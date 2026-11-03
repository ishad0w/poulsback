// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    gl_FragColor = v;
    gl_FragDepth = clamp(v.z * 0.5 + 0.5, 0.0, 1.0);
}
