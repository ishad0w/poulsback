// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
void main()
{
    gl_FragColor = vec4(fract(gl_FragCoord.x / 16.0), fract(gl_FragCoord.y / 16.0), gl_FragCoord.z,
                        gl_FrontFacing ? 1.0 : 0.25);
}
