// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    vec2 q = v.xy * v.zw;
    gl_FragColor = vec4(fract(dFdx(q.x) * 4.0), fract(dFdy(q.y) * 4.0), fract(fwidth(v.z)),
                        fract(dFdx(v.w) + dFdy(v.x)));
}
