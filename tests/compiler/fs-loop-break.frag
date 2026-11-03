// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform int count;
uniform vec4 k;
varying vec4 v;
void main()
{
    vec4 acc = v;
    for (int i = 0; i < 32; i++) {
        if (i >= count)
            break;
        acc = fract(acc * 1.5 + k);
    }
    gl_FragColor = acc;
}
