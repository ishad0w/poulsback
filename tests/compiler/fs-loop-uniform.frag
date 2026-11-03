// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform float n;
uniform vec4 k;
varying vec4 v;
void main()
{
    vec4 acc = v;
    for (float i = 0.0; i < n; i += 1.0)
        acc = acc * k + vec4(0.25);
    gl_FragColor = fract(acc);
}
