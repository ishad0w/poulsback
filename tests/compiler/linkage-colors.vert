#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

attribute vec4 generic_value;
varying vec4 generic_color;
void main()
{
    gl_Position = gl_Vertex;
    gl_FrontColor = gl_Color;
    gl_FrontSecondaryColor = gl_SecondaryColor;
    generic_color = generic_value;
}
