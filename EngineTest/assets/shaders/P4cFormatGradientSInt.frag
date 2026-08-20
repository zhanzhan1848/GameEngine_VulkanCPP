#version 450
// P4c-F1 format parity: deterministic integer pattern for SInt attachment
// formats. Values signed so negative encoding paths are exercised.
layout(location = 0) out ivec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) & 127;
    outColor = ivec4(p.x, p.y, -128, 32767);
}
