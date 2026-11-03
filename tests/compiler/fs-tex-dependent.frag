// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler2D s, t;
varying vec4 v;
void main()
{
    vec4 a = texture2D(s, v.xy * 2.0 + 0.1);
    gl_FragColor = texture2D(t, a.xy) * a.w + texture2D(s, v.zw, 1.5);
}
