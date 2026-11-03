#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

attribute vec4 position;
varying vec4 color;
void main()
{
    gl_Position = position;
    color = position;
}
