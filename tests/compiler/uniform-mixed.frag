#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

uniform float opacity;
uniform vec2 bias;
uniform vec3 rgb;
uniform vec4 tint;
void main()
{
    gl_FragColor = vec4(opacity, bias.y, rgb.z, tint.w);
}
