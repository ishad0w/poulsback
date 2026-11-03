// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler1D a;
varying vec4 v;
void main()
{
    gl_FragColor = texture1D(a, v.x) + texture1D(a, v.y, 0.5) * 0.5;
}
