#version 450
// P4c-F1 format parity: deterministic integer pattern for UInt attachment
// formats. Encodes pixel coordinates + layer id so CPU reference is exact.
layout(location = 0) out uvec4 outColor;

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy) & 255u;
    outColor = uvec4(p.x, p.y, 128u, 65535u);
}
