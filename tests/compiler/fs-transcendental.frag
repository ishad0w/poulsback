// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
void main()
{
    float x = abs(v.x) + 0.1;
    gl_FragColor = vec4(fract(pow(x, v.y + 1.0)), fract(exp(v.z) + log(x)),
                        fract(sqrt(x) + inversesqrt(x)), fract(exp2(v.w) + log2(x)));
}
