// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler3D b;
varying vec4 v;
void main()
{
    gl_FragColor = texture3D(b, v.xyz);
}
