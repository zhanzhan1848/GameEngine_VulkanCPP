#version 450 core
// LightmapUV pre-pass 像素着色器（2026-10-19 计划，M3）。Entry: main。
// 输出 RGBA16F：xy = atlas UV（已含实体 scale/bias），z = 有效位（1.0），
// a = 1。RT 清零（z=0）表示该像素无 lightmap——composite 据此走原路径。

layout(location = 0) in vec2 vAtlasUV;
layout(location = 0) out vec4 outUV;

void main() {
    outUV = vec4(vAtlasUV, 1.0, 1.0);
}
