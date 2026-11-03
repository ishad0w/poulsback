// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef DEPTH_CUBE_REFERENCE_H
#define DEPTH_CUBE_REFERENCE_H
#include <stdint.h>

/* Ordinary six-face geometry. The three view rows are mutually orthogonal;
 * independent axis scales give a W1 orthographic projection. All source and
 * viewport operations below are exact binary fractions. This is test input,
 * never a native shader or a physical depth-format contract. */
#define DEPTH_CUBE_VERTICES 36u
static void depth_cube_input(uint32_t ordinal, float clip[4], float color[4])
{
    static const int8_t corners[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                         {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    static const uint8_t faces[6][4] = {{0, 3, 7, 4}, {1, 5, 6, 2}, {0, 4, 5, 1},
                                        {3, 2, 6, 7}, {0, 1, 2, 3}, {4, 7, 6, 5}};
    static const uint8_t triangles[6] = {0, 1, 2, 0, 2, 3};
    static const uint8_t colors[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                         {1, 1, 0}, {1, 0, 1}, {0, 1, 1}};
    uint32_t face = ordinal / 6u;
    const int8_t *p = corners[faces[face][triangles[ordinal % 6u]]];
    clip[0] = (float)(p[0] - p[2]) * .25f;
    clip[1] = -(float)(p[0] + 2 * p[1] + p[2]) * .125f;
    clip[2] = (float)(p[0] - p[1] + p[2]) * .125f;
    clip[3] = 1.f;
    for (uint32_t c = 0; c < 3; c++)
        color[c] = (float)colors[face][c];
    color[3] = 1.f;
}
#endif
