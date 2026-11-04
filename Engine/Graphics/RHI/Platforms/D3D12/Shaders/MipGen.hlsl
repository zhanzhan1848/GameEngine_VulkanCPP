// MipGen.hlsl — RHI GenerateMipmaps 的 D3D12 像素着色下采样（R13/T2）。
//
// 方案（canonical PS downsample）：每级 mip 一次全屏三角形 draw——
// src mip 经 SRV 绑 t0、dst mip 经 RTV 绑 OM；PS 对 src 双线性采样，
// 半尺寸目标上的纹素中心恰好落在 src 2x2 块中心 → 硬件 bilinear 即
// 2x2 box 均值（与 Vulkan 侧 vkCmdBlitImage LINEAR blit 链同档滤波）。
//
// 惯例对齐 legacy Engine/Graphics/Direct3D12/Shaders/FullScreenTriangle.hlsl：
// SV_VertexID 无顶点缓冲全屏三角形（位技巧生成 UV），noperspective 插值；
// 语义命名对齐 EngineTest/TestShader.hlsl（SV_POSITION/SV_TARGET）。
// 着色器只用 SM5.0 基础特性（dxc -T vs_6_0/ps_6_0 与 fxc /T vs_5_0/ps_5_0
// 双通道可编——见 Engine/CMakeLists.txt 编译块）。
Texture2D SrcMip : register(t0);
SamplerState LinearSampler : register(s0);

struct MipGenVSOut
{
    noperspective float4 Position : SV_POSITION;
    noperspective float2 UV : TEXCOORD0;
};

MipGenVSOut MipGenVS(in uint VertexIdx : SV_VertexID)
{
    MipGenVSOut output;
    // 3 顶点覆盖 [-1,1]^2 全屏（第 4 个隐藏顶点由屏幕外裁剪补足）。
    const float2 tex = float2(uint2(VertexIdx, VertexIdx << 1) & 2);
    output.Position = float4(lerp(float2(-1, 1), float2(1, -1), tex), 0, 1);
    output.UV = tex;
    return output;
}

float4 MipGenPS(MipGenVSOut input) : SV_TARGET
{
    return SrcMip.Sample(LinearSampler, input.UV);
}
