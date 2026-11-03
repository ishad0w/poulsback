// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 6; i++)
        acc += v * float(i) * 0.1;
    gl_FragColor = fract(acc);
}
