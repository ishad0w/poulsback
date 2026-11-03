// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform vec4 a;
void main()
{
    vec4 c = v;
    if (v.x > 0.25) {
        c.x = c.y * 2.0;
        if (v.y < 0.0)
            c.z = a.z;
    } else
        c = c.wzyx * a;
    gl_FragColor = fract(c);
}
