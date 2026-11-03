// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
uniform sampler2D s;
varying vec4 v;
void main()
{
    gl_FragColor = texture2DProj(s, vec3(v.xy, v.z + 2.0)) + texture2DProj(s, vec4(v.xy, 0.0, v.w + 2.0));
}
