// Vulkan reference: Nanite/CardCapture.frag (main).
#include "../Common/TextureDimensions.hlsli"

struct MaterialInfo
{
    uint albedo_texture_idx;
    float3 albedo_tint;
    float2 uv_scale;
    uint flags;
};

struct VSOut
{
    float2 uv;
    float3 normal;
    uint material_id;
    float3 world_pos;
};

ByteAddressBuffer mdb : register(t9, space0);
cbuffer CardParams : register(b0, space0)
{
    row_major float4x4 cb_view : packoffset(c0);
    row_major float4x4 cb_proj : packoffset(c4);
    float4 cb_center : packoffset(c8);
    float4 cb_fwd : packoffset(c9);
    float4 cb_misc : packoffset(c10);
};

Texture2DArray<float4> albedo_textures : register(t10, space0);
SamplerState texture_sampler : register(s11, space0);

static VSOut IN;

struct StageInput
{
    float2 VSOut_uv : TEXCOORD0;
    float3 VSOut_normal : TEXCOORD1;
    nointerpolation uint VSOut_material_id : TEXCOORD2;
    float3 VSOut_world_pos : TEXCOORD3;
};

struct StageOutput
{
    float4 out_albedo : SV_Target0;
    float4 out_normal : SV_Target1;
    float out_axis_depth : SV_Target2;
};

MaterialInfo load_material(uint idx)
{
    uint base = idx * 12u;
    MaterialInfo m;
    m.albedo_texture_idx = mdb.Load((base + 0u) * 4);
    m.albedo_tint = float3(asfloat(mdb.Load((base + 3u) * 4)), asfloat(mdb.Load((base + 4u) * 4)), asfloat(mdb.Load((base + 5u) * 4)));
    m.flags = mdb.Load((base + 11u) * 4);
    m.uv_scale = float2(asfloat(mdb.Load((base + 9u) * 4)), asfloat(mdb.Load((base + 10u) * 4)));
    return m;
}

uint encode_card_transport_alpha(uint flags) {
    uint transmission8 = (flags >> 8u) & 0xFFu;
    uint transmission4 = (transmission8 + 8u) / 17u;
    uint thickness8 = (flags >> 16u) & 0xFFu;
    uint thickness3 = (thickness8 + 18u) / 36u;
    return 0x80u | (transmission4 << 3u) | thickness3;
}

float2 octEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * lerp((float2)(-1.0), (float2)(1.0), step((float2)(0.0), result.xy));
    }
    return result * 0.5 + 0.5;
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    IN.uv = input.VSOut_uv;
    IN.normal = input.VSOut_normal;
    IN.material_id = input.VSOut_material_id;
    IN.world_pos = input.VSOut_world_pos;
    uint _176;
    mdb.GetDimensions(_176);
    _176 = (_176 - 0) / 4;
    if (IN.material_id >= (uint(int(_176)) / 12u))
    {
        discard;
    }
    MaterialInfo mat = load_material(IN.material_id);
    if ((((((any(isnan(mat.albedo_tint))) || (any(isinf(mat.albedo_tint)))) || (any(isnan(mat.uv_scale)))) || (any(isinf(mat.uv_scale)))) || (any(bool3(mat.albedo_tint.x < 0.0f.xxx.x, mat.albedo_tint.y < 0.0f.xxx.y, mat.albedo_tint.z < 0.0f.xxx.z)))))
    {
        discard;
    }
    float2 scaled_uv = frac(IN.uv * mat.uv_scale);

    int albedo_layers = int3(textureDimensions(albedo_textures, 0u)).z;
    float4 albedo_sample = 1.0f.xxxx;
    if (mat.albedo_texture_idx != 4294967295u)
    {
        if (mat.albedo_texture_idx >= uint(albedo_layers))
        {
            discard;
        }
        albedo_sample = albedo_textures.Sample(texture_sampler, float3(scaled_uv, float(mat.albedo_texture_idx)));
    }
    float3 base_color = albedo_sample.xyz * mat.albedo_tint;
    float axis_depth = dot(IN.world_pos - cb_center.xyz, cb_fwd.xyz);
    bool _352 = ((((((any(isnan(base_color))) || (any(isinf(base_color)))) || (any(isnan(IN.normal)))) || (any(isinf(IN.normal)))) || (dot(IN.normal, IN.normal) < 1e-08f)) || (isnan(axis_depth))) || (isinf(axis_depth));
    if (_352 || (axis_depth < 0.0f))
    {
        discard;
    }
    float3 N = normalize(IN.normal);
    if (dot(N, cb_fwd.xyz) > 0.0f)
    {
        N = -N;
    }
    output.out_albedo = float4(base_color, float(encode_card_transport_alpha(mat.flags)) / 255.0f);
    output.out_normal = float4(octEncode(N), 0.0f, 1.0f);
    output.out_axis_depth = axis_depth;
    return output;
}
