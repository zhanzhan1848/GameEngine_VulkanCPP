// Vulkan reference: PFX/ParticleFXDecal.frag (main).
cbuffer VP : register(b2, space0)
{
    row_major float4x4 vp_inv_view_proj : packoffset(c0);
    float4 vp_screen_size : packoffset(c4);
    float4 vp_decal_color : packoffset(c5);
    float4 vp_pad : packoffset(c6);
};

Texture2D<float4> scene_depth : register(t0, space0);
SamplerState _scene_depth_sampler : register(s0, space0);

struct StageInput
{
    float2 in_uv : TEXCOORD0;
    float in_radius : TEXCOORD1;
    float4 in_center : TEXCOORD2;
    float4 in_color : TEXCOORD3;
    float in_softness : TEXCOORD4;
};

struct StageOutput
{
    float4 out_color : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float scene_d = scene_depth.Sample(_scene_depth_sampler, input.in_uv).x;
    if (scene_d >= 1.0f)
    {
        discard;
    }
    float4 ndc = float4((input.in_uv.x * 2.0f) - 1.0f, (input.in_uv.y * 2.0f) - 1.0f, scene_d, 1.0f);
    float4 world = mul(ndc, vp_inv_view_proj);
    world /= world.w.xxxx;
    float dist = length(world.xyz - input.in_center.xyz);
    float t = clamp(dist / input.in_radius, 0.0f, 1.0f);
    float falloff = 1.0f - smoothstep(1.0f - input.in_softness, 1.0f, t);
    if (falloff <= 0.0f)
    {
        discard;
    }
    output.out_color = float4((input.in_color.xyz * falloff) * input.in_color.w, falloff);
    return output;
}
