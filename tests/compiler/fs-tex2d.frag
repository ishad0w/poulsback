// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler2D s;
void main()
{
    gl_FragColor = texture2D(s, gl_TexCoord[0].xy);
}
