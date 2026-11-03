// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SCENE_COLOR_H
#define POULSBACK_SCENE_COLOR_H
#include "util/format/u_formats.h"
#include <stdint.h>

#define POULSBACK_SCENE_COLOR_VERSION 1u
/* Actual Gallium color state, independent of the unchanged sample record.
 * Dither is not a quantization policy: neither bit value claims a native
 * PACK rule. No target pointer, native format alias or default is retained. */
struct poulsback_scene_color_state {
    uint32_t version, format, dither, reserved;
};
/* The scene's colour format of a target: BGRX (24-bit visuals) is the same
 * 32-bit pixel as BGRA; the hardware writes the unused byte, nobody reads it. */
static inline uint32_t poulsback_scene_color_format(uint32_t pipe_format)
{
    return pipe_format == PIPE_FORMAT_B8G8R8X8_UNORM ? PIPE_FORMAT_B8G8R8A8_UNORM : pipe_format;
}
#endif
