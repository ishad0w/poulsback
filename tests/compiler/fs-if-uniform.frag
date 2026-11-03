// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform float mode;
uniform vec4 a, b;
varying vec4 v;
void main()
{
    vec4 c;
    if (mode > 0.5)
        c = v * a;
    else
        c = v + b;
    gl_FragColor = fract(c);
}
