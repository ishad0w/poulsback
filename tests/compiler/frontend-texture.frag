#version 120
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

uniform sampler2D image;
void main()
{
    gl_FragColor = texture2D(image, gl_Color.xy);
}
