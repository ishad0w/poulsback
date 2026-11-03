// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
varying vec4 v;
varying vec3 n;
uniform vec3 l;
void main()
{
    vec3 nn = normalize(n + vec3(0.0, 0.0, 2.0));
    float d = max(dot(nn, normalize(l + vec3(0.1))), 0.0);
    vec3 r = reflect(-l, nn);
    gl_FragColor = vec4(d, clamp(length(r) * 0.3, 0.0, 1.0),
                        smoothstep(0.0, 1.0, v.x) * step(0.2, v.y), mix(v.z, v.w, 0.3));
}
