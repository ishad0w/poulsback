// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler2D s;
varying vec4 v;
void main()
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; i++)
        sum += texture2D(s, v.xy + vec2(float(i) * 0.1, 0.0));
    gl_FragColor = sum * 0.25;
}
