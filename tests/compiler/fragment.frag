#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

varying vec4 color;
uniform vec4 scale;
uniform vec4 bias;
void main()
{
    gl_FragColor = color * scale + bias;
}
