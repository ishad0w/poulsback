// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform mat3 m3;
uniform mat4 m4;
varying vec4 v;
void main()
{
    vec3 a = m3 * v.xyz;
    vec4 b = v * m4;
    gl_FragColor = fract(vec4(a, 0.0) + b);
}
