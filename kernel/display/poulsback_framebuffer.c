// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Typed DRM framebuffers over SCANOUT BOs, shown by the PSB provider. */
#include <linux/err.h>
#include "poulsback_errsite.h"
#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_lock.h>
#include "poulsback_bo.h"
#include "poulsback_display.h"
#include "poulsback_framebuffer.h"
#include "poulsback_vm_layout.h"

struct poulsback_framebuffer {
	struct drm_framebuffer base;
	struct poulsback_framebuffer *self;
	struct drm_device *dev;
	struct drm_gem_object *object;
	struct poulsback_display_layout view;
	struct poulsback_display_image display;
	struct poulsback_framebuffer_report report;
	struct mutex lock;
	struct list_head retained;
	struct delayed_work retire_work;
	u32 retire_polls;
	bool listed;
};
static LIST_HEAD(retained_framebuffers);
static DEFINE_SPINLOCK(retained_framebuffers_lock);
static const struct drm_framebuffer_funcs framebuffer_funcs;

/* A SCANOUT BO has no layout of its own: the command supplies it. One LINEAR
 * X/ARGB8888 plane at offset 0, pitch a multiple of 64, every row in the BO. */
static int command_matches(struct drm_gem_object *obj, const struct drm_mode_fb_cmd2 *cmd,
			   struct poulsback_display_layout *view)
{
	u32 i;
	if (!poulsback_bo_is_scanout(obj) || cmd->flags ||
	    (cmd->pixel_format != DRM_FORMAT_XRGB8888 &&
	     cmd->pixel_format != DRM_FORMAT_ARGB8888) ||
	    !poulsback_vm_scanout_view_ok(cmd->width, cmd->height, cmd->pitches[0], obj->size))
		return POULSBACK_ERRSITE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(cmd->handles); i++)
		if (cmd->offsets[i] || cmd->modifier[i] ||
		    (i && (cmd->handles[i] || cmd->pitches[i])))
			return POULSBACK_ERRSITE(-EINVAL);
	*view = (struct poulsback_display_layout){.width = cmd->width,
						  .height = cmd->height,
						  .stride = cmd->pitches[0],
						  .format = cmd->pixel_format,
						  .size = obj->size};
	return 0;
}

/* Caller holds lock and a live DRM reference, or is the actual destroy callback.
 * Immutable owner identities precede dereferencing any mutable base.obj slot. */
static int owner_valid(struct poulsback_framebuffer *o)
{
	u32 i;
	if (o->self != o || !o->report.object_ref || !o->report.device_ref ||
	    !o->report.module_ref || o->base.dev != o->dev || o->base.obj[0] != o->object ||
	    o->base.funcs != &framebuffer_funcs)
		return POULSBACK_ERRSITE(-ESTALE);
	if (!poulsback_bo_is_scanout(o->object) || o->object->dev != o->dev)
		return POULSBACK_ERRSITE(-EXDEV);
	if (o->view.size != o->object->size || o->base.width != o->view.width ||
	    o->base.height != o->view.height || o->base.pitches[0] != o->view.stride ||
	    o->base.format != drm_format_info(o->view.format) || o->base.flags || o->base.modifier)
		return POULSBACK_ERRSITE(-ESTALE);
	for (i = 0; i < ARRAY_SIZE(o->base.obj); i++)
		if (o->base.offsets[i] || (i && (o->base.obj[i] || o->base.pitches[i])))
			return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}

/* Lifecycle counts for the display debugfs file: how far framebuffers got. */
static atomic_t fb_created, fb_destroyed, fb_retire_queued, fb_freed, fb_held;
void poulsback_framebuffer_counts(u32 out[5])
{
	out[0] = atomic_read(&fb_created);
	out[1] = atomic_read(&fb_destroyed);
	out[2] = atomic_read(&fb_retire_queued);
	out[3] = atomic_read(&fb_freed);
	out[4] = atomic_read(&fb_held);
}

static void retain_owner(struct poulsback_framebuffer *o, int primary, int cleanup)
{
	if (!o->report.held)
		atomic_inc(&fb_held);
	o->report.held = 1;
	if (primary && !o->report.primary_error)
		o->report.primary_error = primary;
	if (cleanup && !o->report.cleanup_error)
		o->report.cleanup_error = cleanup;
	/* The owner's own reference on o->object, never obj[0], is now kept. */
}

static void retain_unregistered(struct poulsback_framebuffer *o)
{
	unsigned long flags;
	spin_lock_irqsave(&retained_framebuffers_lock, flags);
	if (!o->listed) {
		list_add_tail(&o->retained, &retained_framebuffers);
		o->listed = true;
	}
	spin_unlock_irqrestore(&retained_framebuffers_lock, flags);
}

static void free_owner(struct poulsback_framebuffer *o)
{
	struct drm_gem_object *obj = o->object;
	struct drm_device *dev = o->dev;
	atomic_inc(&fb_freed);
	/* No published display owner, resource, pin or fence may reach this path. */
	drm_gem_object_put(obj);
	drm_dev_put(dev);
	kfree(o);
	module_put(THIS_MODULE);
}

static int retire_display(struct poulsback_framebuffer *o)
{
	int ret;
	if (!o->report.pin_completed)
		return memchr_inv(&o->display, 0, sizeof(o->display)) ? POULSBACK_ERRSITE(-ESTALE) : 0;
	if (o->report.retired)
		return 0;
	if (o->report.retire_attempts == U32_MAX)
		return POULSBACK_ERRSITE(-EOVERFLOW);
	o->report.retire_attempts++;
	ret = poulsback_display_retire(&o->display);
	o->report.retire_error = ret;
	if (!ret)
		o->report.retired = 1;
	return ret;
}

/* A replaced buffer retires two frames after its successor's BASE store, at
 * 50 Hz 40 ms; retirement is polled that long, with margin, before HOLD. */
#define RETIRE_POLL_MS 5
#define RETIRE_POLLS 40

/* The provider's retirement callbacks run under every modeset lock, as from
 * set_config; a destroy may already hold them (a modeset dropping old_fb), so
 * the retirement of a mapped framebuffer runs here. Success frees the owner. */
static void framebuffer_retire_work(struct work_struct *work)
{
	struct poulsback_framebuffer *o =
		container_of(to_delayed_work(work), struct poulsback_framebuffer, retire_work);
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0, locked;
	DRM_MODESET_LOCK_ALL_BEGIN(o->dev, ctx, 0, locked);
	mutex_lock(&o->lock);
	ret = retire_display(o);
	mutex_unlock(&o->lock);
	DRM_MODESET_LOCK_ALL_END(o->dev, ctx, locked);
	ret = locked ?: ret;
	if (ret == -EAGAIN && o->retire_polls < RETIRE_POLLS) {
		o->retire_polls++;
		schedule_delayed_work(&o->retire_work, msecs_to_jiffies(RETIRE_POLL_MS));
		return;
	}
	mutex_lock(&o->lock);
	if (ret || o->report.held) {
		/* HOLD keeps the buffer bound for the boot: say why. */
		retain_owner(o, 0, POULSBACK_ERRSITE(ret ?: -EIO));
		retain_unregistered(o);
		mutex_unlock(&o->lock);
		return;
	}
	mutex_unlock(&o->lock);
	free_owner(o);
}

static void framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct poulsback_framebuffer *o = container_of(fb, struct poulsback_framebuffer, base);
	int ret;
	mutex_lock(&o->lock);
	o->report.destroy_called = 1;
	atomic_inc(&fb_destroyed);
	/* DRM called us at refcount0. Unlink its lists/IDR now, including HOLD:
	 * sleeping retention must never leave a zero-ref DRM base discoverable. */
	drm_framebuffer_cleanup(fb);
	o->report.drm_registered = 0;
	o->report.drm_cleaned = 1;
	ret = o->report.held ? POULSBACK_ERRSITE(-EIO) : owner_valid(o);
	if (!ret && o->report.pin_completed && !o->report.retired) {
		mutex_unlock(&o->lock);
		atomic_inc(&fb_retire_queued);
		schedule_delayed_work(&o->retire_work, 0);
		return;
	}
	if (!ret)
		ret = retire_display(o);
	if (ret || o->report.held) {
		retain_owner(o, 0, ret);
		retain_unregistered(o);
		mutex_unlock(&o->lock);
		return;
	}
	mutex_unlock(&o->lock);
	free_owner(o);
}

static int framebuffer_create_handle(struct drm_framebuffer *fb, struct drm_file *file, u32 *handle)
{
	struct poulsback_framebuffer *o = container_of(fb, struct poulsback_framebuffer, base);
	u32 result = 0;
	int ret;
	if (!file || !file->minor || !handle ||
	    poulsback_overlap(o, sizeof(*o), handle, sizeof(*handle)))
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	ret = file->minor->dev != o->dev		   ? POULSBACK_ERRSITE(-EXDEV)
	      : o->report.destroy_called || o->report.held ? POULSBACK_ERRSITE(-EIO)
							   : owner_valid(o);
	if (ret)
		goto out;
	ret = drm_gem_handle_create(file, o->object, &result);
	if (!ret) {
		/* Handle allocation may sleep. A new alias is not permission to use
		 * a changed image/base; remove only this exact newly-created handle. */
		ret = owner_valid(o);
		if (ret) {
			int cleanup = drm_gem_handle_delete(file, result);
			retain_owner(o, ret, cleanup);
		} else {
			*handle = result;
		}
	}
out:
	mutex_unlock(&o->lock);
	return ret;
}

static const struct drm_framebuffer_funcs framebuffer_funcs = {
	.destroy = framebuffer_destroy,
	.create_handle = framebuffer_create_handle,
};

/* fb_create registers this type over a SCANOUT BO; plane admission is the
 * PSB provider's. The caller holds obj and immutable cmd through prepare. The
 * command is the layout: one LINEAR X/ARGB8888 plane, pitch a multiple of 64,
 * pitch * height within the BO, flags/offsets/unused planes 0.
 * On success the caller owns the initial DRM framebuffer reference. A retained
 * partial owner may be returned on failure; only its cached status is usable.
 * No SGX VA, display address or readiness argument is accepted. */
static int poulsback_framebuffer_prepare(struct drm_device *dev, struct drm_gem_object *obj,
					 const struct drm_mode_fb_cmd2 *request,
					 struct poulsback_framebuffer **out)
{
	struct drm_mode_fb_cmd2 cmd;
	struct poulsback_display_layout view;
	struct poulsback_framebuffer *o;
	int ret, idx;
	if (!dev || !request || !out || *out || !poulsback_bo_is_scanout(obj))
		return POULSBACK_ERRSITE(-EINVAL);
	if (obj->dev != dev)
		return POULSBACK_ERRSITE(-EXDEV);
	cmd = *request;
	ret = command_matches(obj, &cmd, &view);
	if (ret)
		return ret;
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o) {
		ret = -ENOMEM;
		goto exit;
	}
	if (!try_module_get(THIS_MODULE)) {
		kfree(o);
		ret = POULSBACK_ERRSITE(-ENODEV);
		goto exit;
	}
	drm_dev_get(dev);
	drm_gem_object_get(obj);
	o->self = o;
	o->dev = dev;
	o->object = obj;
	o->view = view;
	o->report = (struct poulsback_framebuffer_report){.version = 1,
							  .image_bytes = view.size,
							  .width = view.width,
							  .height = view.height,
							  .stride = view.stride,
							  .format = view.format,
							  .object_ref = 1,
							  .device_ref = 1,
							  .module_ref = 1};
	mutex_init(&o->lock);
	INIT_LIST_HEAD(&o->retained);
	INIT_DELAYED_WORK(&o->retire_work, framebuffer_retire_work);
	if (memcmp(&cmd, request, sizeof(cmd))) {
		ret = POULSBACK_ERRSITE(-ESTALE);
		goto free;
	}
	drm_helper_mode_fill_fb_struct(dev, &o->base, &cmd);
	o->base.obj[0] = obj;
	ret = drm_framebuffer_init(dev, &o->base, &framebuffer_funcs);
	if (ret)
		goto free;
	o->report.drm_registered = 1;
	ret = owner_valid(o);
	if (ret) {
		retain_owner(o, ret, 0);
		*out = o;
		drm_framebuffer_put(&o->base);
		goto exit;
	}
	o->report.prepared = 1;
	*out = o;
	goto exit;
free:
	free_owner(o);
exit:
	drm_dev_exit(idx);
	return ret;
}

static int poulsback_framebuffer_put(struct poulsback_framebuffer *);
struct drm_framebuffer *poulsback_framebuffer_create(struct drm_device *dev,
						     struct drm_gem_object *object,
						     const struct drm_mode_fb_cmd2 *command)
{
	struct poulsback_framebuffer *owner = NULL;
	struct drm_framebuffer *fb;
	int ret = poulsback_framebuffer_prepare(dev, object, command, &owner);
	if (ret)
		return ERR_PTR(ret);
	fb = poulsback_framebuffer_base(owner);
	if (!fb) {
		/* The source can be lost after prepare. Consume only our registered
         * DRM reference; typed destroy keeps an uncertain owner retained. */
		(void)poulsback_framebuffer_put(owner);
		return ERR_PTR(-ESTALE);
	}
	atomic_inc(&fb_created);
	return fb;
}

int poulsback_framebuffer_legacy_check(const struct drm_framebuffer *next,
				       const struct drm_framebuffer *previous)
{
	const struct drm_framebuffer *frames[] = {next, previous};
	u32 i;
	for (i = 0; i < ARRAY_SIZE(frames); i++) {
		const struct drm_framebuffer *fb = frames[i];
		if (!fb)
			continue;
		/* funcs identity precedes every object dereference: a held typed
         * framebuffer may have deliberately unusable mutable obj slots. */
		if (poulsback_framebuffer_is_object(fb))
			return POULSBACK_ERRSITE(-EOPNOTSUPP);
		if (!fb->obj[0])
			return POULSBACK_ERRSITE(-EINVAL);
		if (poulsback_bo_is_object(fb->obj[0]))
			return POULSBACK_ERRSITE(-EOPNOTSUPP);
	}
	return 0;
}

bool poulsback_framebuffer_is_object(const struct drm_framebuffer *fb)
{
	return fb && fb->funcs == &framebuffer_funcs;
}

struct poulsback_framebuffer *poulsback_framebuffer_from_base(struct drm_framebuffer *fb,
							      struct drm_device *dev)
{
	struct poulsback_framebuffer *o;
	int ret;
	/* Authenticate the actual destructor before interpreting the containing
	 * storage. The caller's DRM reference spans this borrowed result. */
	if (!dev || !poulsback_framebuffer_is_object(fb))
		return ERR_PTR(-EINVAL);
	if (fb->dev != dev)
		return ERR_PTR(-EXDEV);
	o = container_of(fb, struct poulsback_framebuffer, base);
	mutex_lock(&o->lock);
	ret = o->self != o || o->dev != dev ? POULSBACK_ERRSITE(-ESTALE) : owner_valid(o);
	if (!ret && (!o->report.prepared || !o->report.drm_registered || o->report.destroy_called ||
		     o->report.held))
		ret = POULSBACK_ERRSITE(-EIO);
	mutex_unlock(&o->lock);
	return ret ? ERR_PTR(ret) : o;
}

struct drm_framebuffer *poulsback_framebuffer_base(struct poulsback_framebuffer *o)
{
	struct drm_framebuffer *fb = NULL;
	if (!o)
		return NULL;
	mutex_lock(&o->lock);
	if (o->self == o && o->report.prepared && o->report.drm_registered &&
	    !o->report.destroy_called && !o->report.held && !owner_valid(o))
		fb = &o->base;
	mutex_unlock(&o->lock);
	return fb;
}

/* Consume one caller-owned live DRM reference, including a held framebuffer
 * whose base() is unavailable. Refcount0/never-registered retained owners return
 * EALREADY. Success may destroy an unheld owner; do not use that pointer again. */
static int poulsback_framebuffer_put(struct poulsback_framebuffer *o)
{
	struct drm_framebuffer *fb;
	int ret = 0;
	if (!o)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	if (o->self != o)
		ret = POULSBACK_ERRSITE(-ESTALE);
	else if (!o->report.drm_registered || o->report.destroy_called)
		ret = -EALREADY;
	fb = &o->base;
	mutex_unlock(&o->lock);
	/* The caller's existing reference protects fb across this unlock. Destroy
	 * takes lock itself and must run without this mutex already held. */
	if (!ret)
		drm_framebuffer_put(fb);
	return ret;
}

struct drm_gem_object *poulsback_framebuffer_object_get(struct drm_framebuffer *fb,
							struct drm_device *dev)
{
	struct poulsback_framebuffer *o;
	struct drm_gem_object *obj;
	int ret;
	if (!poulsback_framebuffer_is_object(fb) || !dev)
		return ERR_PTR(-EINVAL);
	o = container_of(fb, struct poulsback_framebuffer, base);
	mutex_lock(&o->lock);
	ret = o->dev != dev				   ? POULSBACK_ERRSITE(-EXDEV)
	      : o->report.destroy_called || o->report.held ? POULSBACK_ERRSITE(-EIO)
							   : owner_valid(o);
	obj = ret ? ERR_PTR(ret) : o->object;
	if (!ret)
		drm_gem_object_get(obj);
	mutex_unlock(&o->lock);
	return obj;
}
int poulsback_framebuffer_display_owner(struct drm_framebuffer *fb, struct drm_device *dev,
					struct poulsback_display_domain *d,
					struct poulsback_framebuffer_display_owner *out)
{
	struct poulsback_framebuffer *o;
	struct poulsback_display_image *m;
	int ret;
	if (!poulsback_framebuffer_is_object(fb) || !dev || !d || d->dev != dev || !out)
		return POULSBACK_ERRSITE(-EINVAL);
	o = container_of(fb, struct poulsback_framebuffer, base);
	if (poulsback_overlap(out, sizeof(*out), o, sizeof(*o)) ||
	    poulsback_overlap(out, sizeof(*out), d, sizeof(*d)))
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	ret = o->dev != dev ? POULSBACK_ERRSITE(-EXDEV) : owner_valid(o);
	if (!ret && (!o->report.prepared || !o->report.drm_registered || o->report.destroy_called ||
		     o->report.held || !o->report.pin_completed || o->report.retired))
		ret = POULSBACK_ERRSITE(-EIO);
	m = &o->display;
	mutex_lock(&d->lock);
	/* MAPPED, or PENDING: replaced but not yet retired, so it may be shown again. */
	if (!ret &&
	    (!d->initialized || d->held || m->domain != d || m->object != o->object ||
	     !m->tracked ||
	     (m->phase != POULSBACK_DISPLAY_MAPPED && m->phase != POULSBACK_DISPLAY_PENDING) ||
	     !m->mapped || m->mapped != m->pin.count || !m->read_fence ||
	     dma_fence_is_signaled(m->read_fence)))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret)
		ret = poulsback_bo_display_check(m->pin.object, m->pin.pages, m->pin.count);
	if (!ret)
		ret = owner_valid(o);
	if (!ret)
		*out = (struct poulsback_framebuffer_display_owner){o, m};
	mutex_unlock(&d->lock);
	mutex_unlock(&o->lock);
	return ret;
}

int poulsback_framebuffer_pin(struct poulsback_framebuffer *o, struct poulsback_display_domain *d)
{
	int ret;
	if (!o || !d)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	ret = o->report.destroy_called || o->report.held ? POULSBACK_ERRSITE(-EIO) : owner_valid(o);
	if (ret)
		goto out;
	if (d->dev != o->dev) {
		ret = POULSBACK_ERRSITE(-EXDEV);
		goto out;
	}
	if (o->report.pin_attempted) {
		ret = -EALREADY;
		goto out;
	}
	o->report.pin_attempted = 1;
	ret = poulsback_display_prepare(d, o->object, &o->view, &o->display);
	o->report.pin_error = ret;
	if (!ret)
		o->report.pin_completed = 1;
	else {
		if (!o->report.primary_error)
			o->report.primary_error = ret;
		/* A refusal that left no display state (a full domain: ENOSPC) may
		 * be retried once a slot is free; anything retained holds. */
		if (memchr_inv(&o->display, 0, sizeof(o->display)))
			retain_owner(o, ret, o->display.cleanup_error);
		else
			o->report.pin_attempted = 0;
	}
out:
	mutex_unlock(&o->lock);
	return ret;
}

int poulsback_framebuffer_flip(struct poulsback_framebuffer *o)
{
	int ret;
	if (!o)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	ret = o->report.destroy_called || o->report.held ? POULSBACK_ERRSITE(-EIO) : owner_valid(o);
	if (!ret && (!o->report.pin_completed || o->report.retired))
		ret = POULSBACK_ERRSITE(-EINVAL);
	if (!ret) {
		ret = poulsback_display_flip(o->display.domain, &o->display);
		o->report.flip_error = ret;
		if (ret && ret != -EALREADY)
			retain_owner(o, ret, 0);
	}
	mutex_unlock(&o->lock);
	return ret;
}

int poulsback_framebuffer_status(struct poulsback_framebuffer *o,
				 struct poulsback_framebuffer_report *out)
{
	if (!o || !out || poulsback_overlap(o, sizeof(*o), out, sizeof(*out)))
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&o->lock);
	if (o->self != o) {
		mutex_unlock(&o->lock);
		return POULSBACK_ERRSITE(-ESTALE);
	}
	*out = o->report;
	mutex_unlock(&o->lock);
	return 0;
}
