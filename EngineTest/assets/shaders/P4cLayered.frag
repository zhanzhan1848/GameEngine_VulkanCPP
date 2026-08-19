#version 460
// P4c-F5: per-layer gradient + unique disc marker (isolation check).
layout(location = 0) in flat int vLayer;
layout(location = 0) out vec4 outColor;

void main() {
    float l = float(vLayer);
    vec2 uv = gl_FragCoord.xy / vec2(64.0, 64.0);
    outColor = vec4(l / 3.0, 1.0 - l / 3.0, 0.25 + 0.1 * l, 1.0);
    // layer i 的独有圆盘(中心 x = 0.2 + 0.15i) — 其它层同位置必须无此标记
    vec2 c = vec2(0.2 + 0.15 * l, 0.5);
    vec2 d = uv - c;
    if (dot(d, d) < 0.01) outColor = vec4(1.0, 1.0, 1.0, 1.0);
}
