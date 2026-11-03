// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
void main()
{
    gl_FragColor = gl_Color * gl_SecondaryColor + vec4(gl_TexCoord[1].xy, gl_FogFragCoord, 1.0);
}
