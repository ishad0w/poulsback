// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    gl_FragColor = vec4(asin(clamp(v.x, -1.0, 1.0)) * 0.3 + 0.5, acos(clamp(v.y, -1.0, 1.0)) * 0.3,
                        atan(v.z, v.w + 2.0) * 0.3 + 0.5, atan(v.x) * 0.3 + 0.5);
}
