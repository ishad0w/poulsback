// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform float s;
void main()
{
    float a = v.x * 3.14159 * s;
    gl_FragColor = vec4(sin(a) * 0.5 + 0.5, cos(a) * 0.5 + 0.5, sin(v.y * 10.0) * 0.5 + 0.5,
                        tan(v.z * 0.7) * 0.25 + 0.5);
}
