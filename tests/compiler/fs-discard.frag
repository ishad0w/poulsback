// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
uniform float edge;
void main()
{
    if (v.x < edge)
        discard;
    if (v.y * v.z > 0.5)
        discard;
    gl_FragColor = v;
}
