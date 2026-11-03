// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform samplerCube c;
varying vec4 v;
void main()
{
    gl_FragColor = textureCube(c, v.yzw) * 0.5 + textureCube(c, v.xyz, 1.0) * 0.5;
}
