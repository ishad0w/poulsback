// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/*
 * Parts follow xf86-video-modesetting's drmmode_display.c, which carries:
 *
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2019 NVIDIA CORPORATION
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* The KMS display of the poulsback DDX: xf86Crtc/RandR 1.2 over the legacy
 * KMS ioctls.
 *
 * One xf86 CRTC per CRTC that an encoder of a connected connector can drive
 * (on the Dell: pipe B, for the LVDS panel) and one output per connected
 * connector. No request ever names another CRTC: a disabled pipe's registers
 * hang the machine (HW-DISP-001). The screen keeps the size of the first
 * output's preferred mode: the front is one SCANOUT BO of that size made at
 * screen init, so only modes of that size are valid. The hardware cursor is a 64x64 ARGB gma500
 * dumb buffer; the kernel copies it at every SetCursor, so a new image is set
 * again while shown. Gamma goes through the legacy 256-entry ramp. DPMS is the
 * connector's DPMS property (gma500 switches the panel, pipe and DPLL); while an
 * output is off no cursor or gamma request reaches its pipe, and both are
 * restored once it is on again. */
#include "poulsback_drv.h"
#include "xf86Crtc.h"
#include "xf86Cursor.h"
#include "xf86DDC.h"
#include "xf86Modes.h"
#include <X11/extensions/dpmsconst.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct output_private {
    uint32_t id, dpms;
    drmModeConnectorPtr connector;
    drmModePropertyBlobPtr edid;
};

static const char *const output_names[] = {
    "None", "VGA", "DVI-I", "DVI-D",  "DVI-A", "Composite", "SVIDEO",  "LVDS", "Component",
    "DIN",  "DP",  "HDMI",  "HDMI-B", "TV",    "eDP",       "Virtual", "DSI",  "DPI",
};

static void mode_from_kernel(ScrnInfoPtr scrn, const drmModeModeInfo *k, DisplayModePtr m)
{
    memset(m, 0, sizeof(*m));
    m->status = MODE_OK;
    m->Clock = (int)k->clock;
    m->HDisplay = k->hdisplay;
    m->HSyncStart = k->hsync_start;
    m->HSyncEnd = k->hsync_end;
    m->HTotal = k->htotal;
    m->HSkew = k->hskew;
    m->VDisplay = k->vdisplay;
    m->VSyncStart = k->vsync_start;
    m->VSyncEnd = k->vsync_end;
    m->VTotal = k->vtotal;
    m->VScan = k->vscan;
    m->Flags = (int)k->flags;
    m->name = strdup(k->name);
    if (k->type & DRM_MODE_TYPE_DRIVER)
        m->type = M_T_DRIVER;
    if (k->type & DRM_MODE_TYPE_PREFERRED)
        m->type |= M_T_PREFERRED;
    xf86SetModeCrtc(m, scrn->adjustFlags);
}

static void mode_to_kernel(const DisplayModeRec *m, drmModeModeInfo *k)
{
    memset(k, 0, sizeof(*k));
    k->clock = (uint32_t)m->Clock;
    k->hdisplay = (uint16_t)m->HDisplay;
    k->hsync_start = (uint16_t)m->HSyncStart;
    k->hsync_end = (uint16_t)m->HSyncEnd;
    k->htotal = (uint16_t)m->HTotal;
    k->hskew = (uint16_t)m->HSkew;
    k->vdisplay = (uint16_t)m->VDisplay;
    k->vsync_start = (uint16_t)m->VSyncStart;
    k->vsync_end = (uint16_t)m->VSyncEnd;
    k->vtotal = (uint16_t)m->VTotal;
    k->vscan = (uint16_t)m->VScan;
    k->flags = (uint32_t)m->Flags;
    if (m->name)
        strncpy(k->name, m->name, DRM_DISPLAY_MODE_LEN - 1);
}

/* While the outputs are off the ramp waits in crtc->gamma_* (output_dpms). */
static void crtc_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue, int size)
{
    struct poulsback_crtc *c = crtc->driver_private;
    if (!c->off)
        drmModeCrtcSetGamma(pb_of(crtc->scrn)->fd, c->id, (uint32_t)size, red, green, blue);
}

/* DPMS is per output (output_dpms). */
static void crtc_dpms(xf86CrtcPtr crtc, int mode)
{
    (void)crtc;
    (void)mode;
}

static Bool crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode, Rotation rotation, int x,
                                int y)
{
    ScrnInfoPtr scrn = crtc->scrn;
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    struct poulsback *pb = pb_of(scrn);
    struct poulsback_crtc *c = crtc->driver_private;
    DisplayModeRec saved_mode = crtc->mode;
    int saved_x = crtc->x, saved_y = crtc->y, n = 0;
    Rotation saved_rotation = crtc->rotation;
    uint32_t connectors[8];
    drmModeModeInfo k;
    if (rotation != RR_Rotate_0 || x || y || mode->HDisplay != pb->width ||
        mode->VDisplay != pb->height || !pb->fb)
        return FALSE;
    for (int i = 0; i < config->num_output && n < 8; i++)
        if (config->output[i]->crtc == crtc)
            connectors[n++] = ((struct output_private *)config->output[i]->driver_private)->id;
    /* The server's cursor and RandR code read the CRTC's mode, position and
     * transform (xf86CrtcRotate, identity here). */
    crtc->mode = *mode;
    crtc->x = x;
    crtc->y = y;
    crtc->rotation = rotation;
    mode_to_kernel(mode, &k);
    if (!n || !xf86CrtcRotate(crtc) ||
        drmModeSetCrtc(pb->fd, c->id, pb->fb, 0, 0, connectors, n, &k)) {
        pb->mode_error = n ? errno : ENODEV;
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "SetCrtc %u (%s): %s\n", c->id, mode->name,
                   n ? strerror(pb->mode_error) : "no output");
        crtc->mode = saved_mode;
        crtc->x = saved_x;
        crtc->y = saved_y;
        crtc->rotation = saved_rotation;
        return FALSE;
    }
    crtc_gamma_set(crtc, crtc->gamma_red, crtc->gamma_green, crtc->gamma_blue, crtc->gamma_size);
    crtc->active = TRUE;
    if (scrn->pScreen)
        xf86CrtcSetScreenSubpixelOrder(scrn->pScreen);
    /* A mode set leaves the outputs on, as modesetting does. */
    for (int i = 0; i < config->num_output; i++)
        if (config->output[i]->crtc == crtc)
            config->output[i]->funcs->dpms(config->output[i], DPMSModeOn);
    return TRUE;
}

static void crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
    struct poulsback_crtc *c = crtc->driver_private;
    c->cursor_x = x;
    c->cursor_y = y;
    if (!c->off)
        drmModeMoveCursor(pb_of(crtc->scrn)->fd, c->id, x, y);
}

/* FALSE makes the server fall back to the software cursor (said once). */
static Bool set_cursor(xf86CrtcPtr crtc)
{
    static Bool reported;
    struct poulsback *pb = pb_of(crtc->scrn);
    struct poulsback_crtc *c = crtc->driver_private;
    int ret = drmModeSetCursor(pb->fd, c->id, pb->cursor_handle, POULSBACK_CURSOR_SIZE,
                               POULSBACK_CURSOR_SIZE);
    if (ret && !reported) {
        reported = TRUE;
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "hardware cursor refused (%s): software cursor\n", strerror(-ret));
    }
    return !ret;
}

static Bool crtc_show_cursor_check(xf86CrtcPtr crtc)
{
    struct poulsback_crtc *c = crtc->driver_private;
    c->cursor_up = TRUE;
    return c->off || set_cursor(crtc);
}

static void crtc_hide_cursor(xf86CrtcPtr crtc)
{
    struct poulsback_crtc *c = crtc->driver_private;
    c->cursor_up = FALSE;
    if (!c->off)
        drmModeSetCursor(pb_of(crtc->scrn)->fd, c->id, 0, POULSBACK_CURSOR_SIZE,
                         POULSBACK_CURSOR_SIZE);
}

static Bool crtc_load_cursor_argb_check(xf86CrtcPtr crtc, CARD32 *image)
{
    struct poulsback *pb = pb_of(crtc->scrn);
    struct poulsback_crtc *c = crtc->driver_private;
    if (!pb->cursor)
        return FALSE;
    memcpy(pb->cursor, image, POULSBACK_CURSOR_SIZE * POULSBACK_CURSOR_SIZE * 4);
    return !c->cursor_up || c->off || set_cursor(crtc);
}

static void crtc_destroy(xf86CrtcPtr crtc)
{
    free(crtc->driver_private);
    crtc->driver_private = NULL;
}

static const xf86CrtcFuncsRec crtc_funcs = {
    .dpms = crtc_dpms,
    .set_mode_major = crtc_set_mode_major,
    .set_cursor_position = crtc_set_cursor_position,
    .show_cursor_check = crtc_show_cursor_check,
    .hide_cursor = crtc_hide_cursor,
    .load_cursor_argb_check = crtc_load_cursor_argb_check,
    .gamma_set = crtc_gamma_set,
    .destroy = crtc_destroy,
};

static xf86OutputStatus output_detect(xf86OutputPtr output)
{
    struct output_private *o = output->driver_private;
    drmModeFreeConnector(o->connector);
    o->connector = drmModeGetConnector(pb_of(output->scrn)->fd, o->id);
    if (!o->connector)
        return XF86OutputStatusDisconnected;
    switch (o->connector->connection) {
    case DRM_MODE_CONNECTED:
        return XF86OutputStatusConnected;
    case DRM_MODE_DISCONNECTED:
        return XF86OutputStatusDisconnected;
    default:
        return XF86OutputStatusUnknown;
    }
}

static int output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
    struct poulsback *pb = pb_of(output->scrn);
    return mode->HDisplay == pb->width && mode->VDisplay == pb->height ? MODE_OK : MODE_BAD;
}

/* The id of the connector's property NAME (0: none) and its value. */
static uint32_t connector_property(int fd, drmModeConnectorPtr k, const char *name, uint64_t *value)
{
    uint32_t id = 0;
    for (int i = 0; i < k->count_props && !id; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, k->props[i]);
        if (p && !strcmp(p->name, name)) {
            id = p->prop_id;
            *value = k->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    return id;
}

static drmModePropertyBlobPtr edid_blob(int fd, drmModeConnectorPtr k)
{
    uint64_t blob = 0;
    return connector_property(fd, k, "EDID", &blob) && blob
               ? drmModeGetPropertyBlob(fd, (uint32_t)blob)
               : NULL;
}

static DisplayModePtr output_get_modes(xf86OutputPtr output)
{
    struct output_private *o = output->driver_private;
    DisplayModePtr modes = NULL;
    xf86MonPtr monitor = NULL;
    if (!o->connector)
        return NULL;
    /* xf86InterpretEDID keeps a pointer to the blob's data: it lives until the next call. */
    drmModeFreePropertyBlob(o->edid);
    o->edid = edid_blob(pb_of(output->scrn)->fd, o->connector);
    if (o->edid && o->edid->length >= 128) {
        monitor = xf86InterpretEDID(output->scrn->scrnIndex, o->edid->data);
        if (monitor && o->edid->length > 128)
            monitor->flags |= MONITOR_EDID_COMPLETE_RAWDATA;
    }
    xf86OutputSetEDID(output, monitor);
    for (int i = 0; i < o->connector->count_modes; i++) {
        DisplayModePtr m = xnfalloc(sizeof(*m));
        mode_from_kernel(output->scrn, &o->connector->modes[i], m);
        modes = xf86ModesAdd(modes, m);
    }
    return modes;
}

/* Off: the CRTC stops taking cursor and gamma requests first. On: once the
 * connector (and with it the pipe) is on, the ramp and cursor are restored. */
static void output_dpms(xf86OutputPtr output, int mode)
{
    struct output_private *o = output->driver_private;
    xf86CrtcPtr crtc = output->crtc;
    struct poulsback_crtc *c = crtc ? crtc->driver_private : NULL;
    int fd = pb_of(output->scrn)->fd;
    if (!o->dpms)
        return;
    if (c && mode != DPMSModeOn)
        c->off = TRUE;
    drmModeConnectorSetProperty(fd, o->id, o->dpms, (uint64_t)mode);
    if (c && mode == DPMSModeOn && c->off) {
        c->off = FALSE;
        crtc_gamma_set(crtc, crtc->gamma_red, crtc->gamma_green, crtc->gamma_blue,
                       crtc->gamma_size);
        if (c->cursor_up && set_cursor(crtc))
            drmModeMoveCursor(fd, c->id, c->cursor_x, c->cursor_y);
    }
}

static void output_destroy(xf86OutputPtr output)
{
    struct output_private *o = output->driver_private;
    drmModeFreePropertyBlob(o->edid);
    drmModeFreeConnector(o->connector);
    free(o);
    output->driver_private = NULL;
}

static const xf86OutputFuncsRec output_funcs = {
    .dpms = output_dpms,
    .mode_valid = output_mode_valid,
    .detect = output_detect,
    .get_modes = output_get_modes,
    .destroy = output_destroy,
};

/* The screen never changes size: one front of the panel's size. */
static Bool config_resize(ScrnInfoPtr scrn, int width, int height)
{
    struct poulsback *pb = pb_of(scrn);
    return width == pb->width && height == pb->height;
}

static const xf86CrtcConfigFuncsRec config_funcs = {.resize = config_resize};

/* The size of a connector's preferred mode (or its first). */
static void preferred_size(drmModeConnectorPtr k, int *width, int *height)
{
    int m = 0;
    for (int i = 0; i < k->count_modes; i++)
        if (k->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            m = i;
            break;
        }
    *width = k->modes[m].hdisplay;
    *height = k->modes[m].vdisplay;
}

Bool poulsback_display_pre_init(ScrnInfoPtr scrn, struct poulsback *pb)
{
    drmModeResPtr res = drmModeGetResources(pb->fd);
    drmModeConnectorPtr connected[8];
    uint32_t masks[8], used = 0;
    int n = 0, crtc_index[32], crtcs = 0;
    Bool ok = FALSE;
    if (!res)
        return FALSE;
    xf86CrtcConfigInit(scrn, &config_funcs);
    /* Connected connectors, and the kernel CRTCs their encoders can drive. */
    for (int i = 0; i < res->count_connectors && n < 8; i++) {
        drmModeConnectorPtr k = drmModeGetConnector(pb->fd, res->connectors[i]);
        uint32_t mask = 0;
        if (!k || k->connection != DRM_MODE_CONNECTED || !k->count_modes) {
            drmModeFreeConnector(k);
            continue;
        }
        for (int e = 0; e < k->count_encoders; e++) {
            drmModeEncoderPtr enc = drmModeGetEncoder(pb->fd, k->encoders[e]);
            if (enc)
                mask |= enc->possible_crtcs;
            drmModeFreeEncoder(enc);
        }
        mask &= (1u << res->count_crtcs) - 1;
        if (!mask) {
            drmModeFreeConnector(k);
            continue;
        }
        if (!n)
            preferred_size(k, &pb->width, &pb->height);
        connected[n] = k;
        masks[n++] = mask;
        used |= mask;
    }
    /* xf86 CRTCs for those kernel CRTCs only; possible_crtcs index them. */
    for (int j = 0; j < res->count_crtcs && j < 32; j++) {
        struct poulsback_crtc *c;
        xf86CrtcPtr crtc;
        crtc_index[j] = -1;
        if (!(used & (1u << j)))
            continue;
        crtc = xf86CrtcCreate(scrn, &crtc_funcs);
        c = calloc(1, sizeof(*c));
        if (!crtc || !c) {
            free(c);
            goto out;
        }
        c->id = res->crtcs[j];
        c->index = (uint32_t)j;
        {
            drmModeCrtcPtr k = drmModeGetCrtc(pb->fd, c->id);
            if (k && k->buffer_id && k->mode_valid) {
                c->initial_fb = k->buffer_id;
                c->initial_mode = k->mode;
            }
            drmModeFreeCrtc(k);
        }
        crtc->driver_private = c;
        crtc_index[j] = crtcs++;
    }
    for (int i = 0; i < n; i++) {
        drmModeConnectorPtr k = connected[i];
        struct output_private *o = calloc(1, sizeof(*o));
        char name[32];
        xf86OutputPtr output;
        if (k->connector_type < ARRAY_SIZE(output_names))
            snprintf(name, sizeof(name), "%s-%u", output_names[k->connector_type],
                     k->connector_type_id);
        else
            snprintf(name, sizeof(name), "Unknown%u-%u", k->connector_type, k->connector_type_id);
        output = o ? xf86OutputCreate(scrn, &output_funcs, name) : NULL;
        if (!output) {
            free(o);
            goto out;
        }
        o->id = k->connector_id;
        o->dpms = connector_property(pb->fd, k, "DPMS", &(uint64_t){0});
        o->connector = k;
        connected[i] = NULL;
        output->driver_private = o;
        output->mm_width = (int)k->mmWidth;
        output->mm_height = (int)k->mmHeight;
        output->interlaceAllowed = output->doubleScanAllowed = FALSE;
        for (int j = 0; j < res->count_crtcs && j < 32; j++)
            if ((masks[i] & (1u << j)) && crtc_index[j] >= 0)
                output->possible_crtcs |= 1u << crtc_index[j];
        xf86DrvMsg(scrn->scrnIndex, X_INFO, "output %s: connector %u\n", name, o->id);
    }
    if (!n || !crtcs) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "no connected output\n");
        goto out;
    }
    xf86CrtcSetSizeRange(scrn, pb->width, pb->height, pb->width, pb->height);
    ok = xf86InitialConfiguration(scrn, TRUE);
out:
    for (int i = 0; i < n; i++)
        drmModeFreeConnector(connected[i]);
    drmModeFreeResources(res);
    return ok;
}

/* The cursor image buffer (a failure leaves the software cursor), then RandR. */
Bool poulsback_display_screen_init(ScreenPtr screen, struct poulsback *pb)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct drm_mode_create_dumb create = {
        .width = POULSBACK_CURSOR_SIZE, .height = POULSBACK_CURSOR_SIZE, .bpp = 32};
    struct drm_mode_map_dumb map = {0};
    void *cursor = MAP_FAILED;
    if (!drmIoctl(pb->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
        pb->cursor_handle = map.handle = create.handle;
        if (!drmIoctl(pb->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
            cursor = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, pb->fd,
                          (off_t)map.offset);
    }
    if (cursor != MAP_FAILED) {
        pb->cursor = cursor;
        if (!xf86_cursors_init(screen, POULSBACK_CURSOR_SIZE, POULSBACK_CURSOR_SIZE,
                               HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
                                   HARDWARE_CURSOR_UPDATE_UNHIDDEN | HARDWARE_CURSOR_ARGB))
            xf86DrvMsg(scrn->scrnIndex, X_WARNING, "hardware cursor setup failed\n");
    } else {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING, "no cursor buffer: %s\n", strerror(errno));
    }
    /* The VT is ours from here on: a cursor first set while vtSema is FALSE
     * stays a software cursor (modesetting sets it at the same point). */
    scrn->vtSema = TRUE;
    return xf86CrtcScreenInit(screen) && xf86DPMSInit(screen, xf86DPMSSet, 0);
}

void poulsback_display_close_screen(ScreenPtr screen, struct poulsback *pb)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    struct drm_mode_destroy_dumb destroy = {.handle = pb->cursor_handle};
    /* The kernel refuses to turn a CRTC off under a typed framebuffer, which
     * RmFB of the shown front would ask: show the initial one first. On
     * another VT the console has it already. */
    for (int i = 0; scrn->vtSema && i < config->num_crtc; i++) {
        struct poulsback_crtc *c = config->crtc[i]->driver_private;
        uint32_t connectors[8];
        int n = 0;
        for (int o = 0; o < config->num_output && n < 8; o++)
            if (config->output[o]->crtc == config->crtc[i])
                connectors[n++] = ((struct output_private *)config->output[o]->driver_private)->id;
        int ret = -1;
        /* A page flip still in flight (EBUSY) completes within a frame. */
        for (int tries = 0; c->initial_fb && n && tries < 5; tries++) {
            ret =
                drmModeSetCrtc(pb->fd, c->id, c->initial_fb, 0, 0, connectors, n, &c->initial_mode);
            if (!ret || errno != EBUSY)
                break;
            usleep(20000);
        }
        if (c->initial_fb && n && ret)
            xf86DrvMsg(scrn->scrnIndex, X_WARNING, "SetCrtc %u back to fb %u: %s\n", c->id,
                       c->initial_fb, strerror(errno));
    }
    xf86_hide_cursors(scrn);
    xf86_cursors_fini(screen);
    if (pb->cursor)
        munmap(pb->cursor, POULSBACK_CURSOR_SIZE * POULSBACK_CURSOR_SIZE * 4);
    if (pb->cursor_handle)
        drmIoctl(pb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    pb->cursor = NULL;
    pb->cursor_handle = 0;
}
