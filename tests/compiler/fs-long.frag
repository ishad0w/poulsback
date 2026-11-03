// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform vec4 k[8];
void main()
{
    vec4 a = v;
    a = a * k[0] + k[1]; a = fract(a * a + k[2]); a = a.yzwx * k[3] - k[4];
    a = max(a, k[5]) + min(a.wzyx, k[6]); a = fract(a * k[7] + a.zxyw);
    a = fract(sqrt(abs(a)) + a * a * a); a = mix(a, a.wzyx, 0.4);
    gl_FragColor = a;
}
