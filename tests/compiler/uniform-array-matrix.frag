#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

uniform vec4 palette[2];
uniform mat2 basis;
void main()
{
    gl_FragColor = vec4(palette[1].x, basis[1].y, palette[0].z, basis[0].x);
}
