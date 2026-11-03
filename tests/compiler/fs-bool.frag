// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform bool flip;
void main()
{
    bvec2 b = lessThan(v.xy, v.zw);
    bool any_ = any(b), all_ = all(b);
    vec4 c = vec4(b.x ? 1.0 : 0.0, any_ ? 0.75 : 0.25, all_ ? 0.6 : 0.1, flip ? v.w : -v.w);
    gl_FragColor = fract(c);
}
