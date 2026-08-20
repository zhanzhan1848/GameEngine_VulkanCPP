#version 450
// P4c-F1 format parity: deterministic gradient + circle geometry, float family
// (UNorm/SNorm/Float attachment formats). Resolution fixed at 64x64 so the
// shader stays backend-independent (gl_FragCoord only).
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(64.0, 64.0);
    vec3 c = vec3(uv.x, uv.y, 0.25 + 0.5 * uv.x * uv.y);
    vec2 d = uv - vec2(0.5, 0.5);
    if (dot(d, d) < 0.15 * 0.15) c = vec3(0.9, 0.1, 0.3);
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
