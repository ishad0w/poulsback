// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler2D s;
uniform float use;
varying vec4 v;
void main()
{
    vec4 c = v;
    if (use > 0.0)
        c = texture2D(s, v.xy);
    gl_FragColor = c;
}
