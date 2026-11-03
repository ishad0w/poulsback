// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    gl_FragColor = gl_FrontFacing ? v : v.wzyx;
}
