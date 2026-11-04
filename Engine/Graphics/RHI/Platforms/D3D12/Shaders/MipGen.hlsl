// MipGen.hlsl - pixel-shader downsampling for RHI GenerateMipmaps on D3D12 (R13/T2).
//
// Approach (canonical PS downsample): one full-screen triangle draw per mip
// level -- the src mip is bound as SRV at t0 and the dst mip as RTV on OM;
// the PS bilinearly samples the src, and texel centers on the half-size
// target land exactly on the centers of the src 2x2 blocks, so hardware
// bilinear equals a 2x2 box average (same filter class as the Vulkan-side
// vkCmdBlitImage LINEAR blit chain).
//
// Conventions follow legacy Engine/Graphics/Direct3D12/Shaders/FullScreenTriangle.hlsl:
// SV_VertexID full-screen triangle with no vertex buffer (bit trick builds
// the UVs), noperspective interpolation; semantics naming follows
// EngineTest/TestShader.hlsl (SV_POSITION/SV_TARGET).
// Precompiled offline by dxc -T vs_6_0 / ps_6_0 and embedded statically via
// D3D12MipGenShaders.h (see scripts/build_d3d12_mipgen.py; R14-2 removed the
// build-time compiler probe chain that triggered 0x80070459 on Windows).
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
    // 3 vertices cover the [-1,1]^2 full screen (the hidden 4th vertex is
    // supplied by off-screen clipping).
    const float2 tex = float2(uint2(VertexIdx, VertexIdx << 1) & 2);
    output.Position = float4(lerp(float2(-1, 1), float2(1, -1), tex), 0, 1);
    output.UV = tex;
    return output;
}

float4 MipGenPS(MipGenVSOut input) : SV_TARGET
{
    return SrcMip.Sample(LinearSampler, input.UV);
}
