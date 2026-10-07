#include <metal_stdlib>
using namespace metal;

// [PassDebugView 排查] 编辑器材质注册用：把任意格式的源纹理（含块压缩）
// 经采样绘制进 RGBA8 纹理数组的目标层视图。Blit 编码器跨格式拷贝不受
// Metal 支持（块尺寸不匹配即跳过），必须走渲染路径。

struct CopyVSOut {
    float4 position [[position]];
    float2 uv;
};

struct CopyConstants {
    float2 uv_scale;   // uv * scale：源尺寸 / 画布尺寸
    float2 _pad;
};

// 全屏三角形（无需顶点缓冲）
vertex CopyVSOut editor_texture_copy_vs(uint vid [[vertex_id]]) {
    CopyVSOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    // 画布 uv 0..1（超出部分被三角形覆盖为越界 uv，由 clamp 采样兜底）
    float2 uv[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };
    out.uv = uv[vid];
    return out;
}

fragment float4 editor_texture_copy_fs(
    CopyVSOut in [[stage_in]],
    texture2d<float> src [[texture(0)]],
    sampler src_sampler [[sampler(0)]],
    constant CopyConstants& constants [[buffer(2)]])
{
    return src.sample(src_sampler, in.uv * constants.uv_scale);
}
