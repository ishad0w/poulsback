#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

void main()
{
    if (gl_Color.r > 0.5)
        gl_FragColor = gl_Color;
    else
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
