// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform int n;
void main()
{
    int k = 0;
    float s = 0.0;
    for (int i = 0; i < n; i++) {
        k += i * 2;
        s += float(k) * 0.01;
    }
    gl_FragColor = vec4(fract(s + v.x), float(k) * 0.01, v.zw);
}
