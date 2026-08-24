#version 450 core
// M5 Pass1(Draw 捕获):RVT 页内容 — 世界空间棋盘(16 世界单位)。
// push:页的世界矩形 [wx0,wz0]-[wx1,wz1](含 border 扩展)。
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Push {
    vec4 worldRect;   // x=wx0 y=wz0 z=wx1 w=wz1
} push;
void main() {
    vec2 w = mix(push.worldRect.xy, push.worldRect.zw, inUV);
    float c = ((int(floor(w.x / 16.0)) + int(floor(w.y / 16.0))) & 1) != 0 ? 0.9 : 0.1;
    outColor = vec4(c, c * 0.75 + 0.125, 1.0 - c * 0.5, 1.0);
}
