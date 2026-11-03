// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    vec4 a = v * 3.7;
    gl_FragColor = fract(vec4(floor(a.x) * 0.1 + ceil(a.y) * 0.07, mod(a.z, 1.3), sign(a.w) * 0.25 + abs(a.x) * 0.1,
                              min(a.x, a.y) + max(a.z, a.w) + clamp(a.y, -0.5, 0.5)));
}
