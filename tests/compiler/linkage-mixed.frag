#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

varying vec4 generic_color;
void main()
{
    gl_FragColor = gl_Color + gl_SecondaryColor + generic_color;
}
