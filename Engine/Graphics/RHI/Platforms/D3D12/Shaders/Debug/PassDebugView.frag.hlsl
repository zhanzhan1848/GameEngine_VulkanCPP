// Vulkan reference: Debug/PassDebugView.frag (main).
ByteAddressBuffer cluster_map : register(t4, space0);
cbuffer PassDebugParams : register(b2, space0)
{
    row_major float4x4 params_inv_view_proj : packoffset(c0);
    row_major float4x4 params_proj : packoffset(c4);
    float4 params_misc : packoffset(c8);
    float4 params_misc2 : packoffset(c9);
};

ByteAddressBuffer shadow_vis : register(t6, space0);
Texture2D<float4> depthTex : register(t3, space0);
SamplerState inputSamp : register(s1, space0);
Texture2D<float4> srcTex : register(t0, space0);
Texture2D<uint4> idTex : register(t5, space0);
SamplerState _idTex_sampler : register(s5, space0);

struct StageInput
{
    float2 inUv : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float ndc_depth_to_linear(float ndcDepth, float nearPlane, float farPlane) {
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcDepth * (farPlane - nearPlane));
}

float3 hash_id_to_color(uint id) {
    uint h = id * 2654435761u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return float3(float(h & 0xFFu) / 255.0, float((h >> 8u) & 0xFFu) / 255.0, float((h >> 16u) & 0xFFu) / 255.0);
}

uint cluster_map_lookup(uint cluster_count, uint meshlet_id, inout uint instance_index)
{
    for (uint i = 0u; i < cluster_count; i++)
    {
        if (cluster_map.Load(((i * 4u) + 0u) * 4) == meshlet_id)
        {
            instance_index = cluster_map.Load(((i * 4u) + 1u) * 4);
            return i;
        }
    }
    instance_index = 4294967295u;
    return 4294967295u;
}

float3 aces_tonemap(float3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float mode = params_misc.z;
    float2 uv = input.inUv;
    float near_p = params_misc.x;
    float far_p = params_misc.y;
    float d = depthTex.Sample(inputSamp, uv).x;
    bool background = d >= 0.9999f;
    if (mode < 0.5f)
    {
        output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
    }
    else
    {
        if (mode < 1.5f)
        {
            float3 c = srcTex.Sample(inputSamp, uv).xyz;
            output.outColor = float4(pow(clamp(c, 0.0f.xxx, 1.0f.xxx), 0.45454547f.xxx), 1.0f);
        }
        else
        {
            if (mode < 2.5f)
            {
                output.outColor = float4(srcTex.Sample(inputSamp, uv).xyz, 1.0f);
            }
            else
            {
                if (mode < 3.5f)
                {
                    output.outColor = float4(srcTex.Sample(inputSamp, uv).xyz, 1.0f);
                }
                else
                {
                    if (mode < 4.5f)
                    {
                        if (background)
                        {
                            output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
                            return output;
                        }
                        float param = d;
                        float param_1 = near_p;
                        float lin = ndc_depth_to_linear(param, param_1, far_p);
                        output.outColor = float4(clamp(lin / far_p, 0.0f, 1.0f).xxx, 1.0f);
                    }
                    else
                    {
                        if (mode < 5.5f)
                        {
                            float3 v = srcTex.Sample(inputSamp, uv).xyz + 0.5f.xxx;
                            output.outColor = float4(clamp(v, 0.0f.xxx, 1.0f.xxx), 1.0f);
                        }
                        else
                        {
                            if (mode < 9.5f)
                            {
                                if (background)
                                {
                                    output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
                                    return output;
                                }
                                int2 pix = int2(uv * params_misc2.xy);
                                uint _packed = idTex.Load(int3(pix, 0)).x;
                                uint tri = _packed & 16777215u;
                                uint meshlet = (_packed >> 24u) & 255u;
                                if (mode < 6.5f)
                                {
                                    if (tri == 0u)
                                    {
                                        output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
                                        return output;
                                    }
                                    output.outColor = float4(hash_id_to_color(tri), 1.0f);
                                }
                                else
                                {
                                    if (mode < 7.5f)
                                    {
                                        if ((meshlet == 0u) && (tri == 0u))
                                        {
                                            output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
                                            return output;
                                        }
                                        output.outColor = float4(hash_id_to_color(meshlet), 1.0f);
                                    }
                                    else
                                    {
                                        if (tri == 0u)
                                        {
                                            output.outColor = float4(0.05f, 0.05f, 0.05f, 1.0f);
                                            return output;
                                        }
                                        uint param_5 = uint(params_misc.w);
                                        uint param_6 = meshlet;
                                        uint param_7;
                                        uint _415 = cluster_map_lookup(param_5, param_6, param_7);
                                        uint instance_index = param_7;
                                        uint entry = _415;
                                        if (entry == 4294967295u)
                                        {
                                            output.outColor = float4((hash_id_to_color(meshlet) * 0.5f) + float3(0.25f, 0.0f, 0.0f), 1.0f);
                                            return output;
                                        }
                                        output.outColor = float4(hash_id_to_color((mode < 8.5f) ? entry : instance_index), 1.0f);
                                    }
                                }
                            }
                            else
                            {
                                if (mode < 10.5f)
                                {
                                    float a = srcTex.Sample(inputSamp, uv).x;
                                    output.outColor = float4(a.xxx, 1.0f);
                                }
                                else
                                {
                                    if (mode < 12.5f)
                                    {
                                        float3 c_1 = srcTex.Sample(inputSamp, uv).xyz;
                                        output.outColor = float4(aces_tonemap(max(c_1, 0.0f.xxx)), 1.0f);
                                    }
                                    else
                                    {
                                        if (mode < 13.5f)
                                        {
                                            float3 c_2 = srcTex.Sample(inputSamp, uv).xyz * params_misc2.z;
                                            output.outColor = float4(aces_tonemap(max(c_2, 0.0f.xxx)), 1.0f);
                                        }
                                        else
                                        {
                                            if (mode < 14.5f)
                                            {
                                                output.outColor = float4(clamp(srcTex.Sample(inputSamp, uv).xyz, 0.0f.xxx, 1.0f.xxx), 1.0f);
                                            }
                                            else
                                            {
                                                if (mode < 15.5f)
                                                {
                                                    float3 c_3 = srcTex.Sample(inputSamp, uv).xyz;
                                                    output.outColor = float4(pow(clamp(c_3, 0.0f.xxx, 1.0f.xxx), 0.45454547f.xxx), 1.0f);
                                                }
                                                else
                                                {
                                                    if (mode < 16.5f)
                                                    {
                                                        float2 f = (srcTex.Sample(inputSamp, uv).xy * 2.0f) - 1.0f.xx;
                                                        float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
                                                        float t = clamp(-n.z, 0.0f, 1.0f);
                                                        float3 _592 = n;
                                                        float3 _596 = n;
                                                        float2 _598 = _596.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _592.xy));
                                                        n.x = _598.x;
                                                        n.y = _598.y;
                                                        output.outColor = float4((normalize(n) * 0.5f) + 0.5f.xxx, 1.0f);
                                                    }
                                                    else
                                                    {
                                                        if (mode < 17.5f)
                                                        {
                                                            float dep = srcTex.Sample(inputSamp, uv).x;
                                                            if (dep < 0.0f)
                                                            {
                                                                output.outColor = float4(0.16f, 0.04f, 0.24f, 1.0f);
                                                            }
                                                            else
                                                            {
                                                                float g = pow(clamp(dep / 2.0f, 0.0f, 1.0f), 0.4f);
                                                                output.outColor = float4(g.xxx, 1.0f);
                                                            }
                                                        }
                                                        else
                                                        {
                                                            if (mode < 18.5f)
                                                            {
                                                                float3 c_4 = srcTex.Sample(inputSamp, uv).xyz * params_misc2.z;
                                                                output.outColor = float4(aces_tonemap(max(c_4, 0.0f.xxx)), 1.0f);
                                                            }
                                                            else
                                                            {
                                                                if (mode < 19.5f)
                                                                {
                                                                    float3 c_5 = srcTex.Sample(inputSamp, uv).xyz * params_misc2.z;
                                                                    output.outColor = float4(aces_tonemap(max(c_5, 0.0f.xxx)), 1.0f);
                                                                }
                                                                else
                                                                {
                                                                    if (mode < 20.5f)
                                                                    {
                                                                        int2 t_1 = int2(uv * params_misc2.w);
                                                                        float v_1 = asfloat(shadow_vis.Load((t_1.x + (t_1.y * int(params_misc2.w))) * 4));
                                                                        output.outColor = float4(clamp(v_1, 0.0f, 1.0f).xxx, 1.0f);
                                                                    }
                                                                    else
                                                                    {
                                                                        if (mode < 21.5f)
                                                                        {
                                                                            float3 c_6 = (srcTex.Sample(inputSamp, uv).xyz * params_misc2.z) * 16.0f;
                                                                            output.outColor = float4(aces_tonemap(max(c_6, 0.0f.xxx)), 1.0f);
                                                                        }
                                                                        else
                                                                        {
                                                                            if (mode < 22.5f)
                                                                            {
                                                                                float3 c_7 = (srcTex.Sample(inputSamp, uv).xyz * params_misc2.z) * 16.0f;
                                                                                output.outColor = float4(aces_tonemap(max(c_7, 0.0f.xxx)), 1.0f);
                                                                            }
                                                                            else
                                                                            {
                                                                                float4 a_1 = srcTex.Sample(inputSamp, uv);
                                                                                output.outColor = float4(clamp(float3(a_1.x, a_1.y, a_1.z / 16.0f), 0.0f.xxx, 1.0f.xxx), 1.0f);
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return output;
}
