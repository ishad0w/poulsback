// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
#extension GL_ARB_texture_rectangle : require
uniform sampler2DRect s;
varying vec4 v;
void main()
{
    gl_FragColor = texture2DRect(s, v.xy * 64.0);
}
