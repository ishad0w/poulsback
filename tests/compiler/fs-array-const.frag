// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform vec4 c[4];
varying vec4 v;
void main()
{
    vec4 t[3];
    t[0] = v;
    t[1] = v * c[1];
    t[2] = t[0] + t[1] + c[3];
    gl_FragColor = fract(t[2] * c[0].x + c[2]);
}
