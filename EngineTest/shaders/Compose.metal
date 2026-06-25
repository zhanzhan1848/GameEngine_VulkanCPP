/**
 * @file Compose.metal
 * @brief 延迟渲染合成着色器 - 从G-Buffer重建场景并进行光照计算
 * @author GameEngine Team
 * @date 2024
 */

#include "Common.h"

/**
 * @brief 顶点着色器输出结构体
 */
struct VertexOutput
{
    float4 position [[position]];
    float2 uv;
};

fragment float4 compose_pass(VertexOutput fragInput [[stage_in]],
                                device GlobalShaderData* globalData [[buffer(0)]],
                                device DirectionalLightParameters* light_params [[buffer(1)]],
                                texture2d<float, access::sample> albedo_texture [[texture(0)]],
                                texture2d<float, access::sample> normal_depth_texture [[texture(1)]],
                                texture2d<float, access::sample> world_pos_texture [[texture(2)]],
                                texture2d_array<float, access::sample> shadow_mapping [[texture(3)]],
                                sampler s [[sampler(0)]])
{
    float2 uv = fragInput.uv;
    uv.y = 1.f - uv.y;
    float3 color = 0.f;
    Surface S;
    float4 albedo = albedo_texture.sample(s, uv);
    float4 normal_depth = normal_depth_texture.sample(s, uv);
    float4 world_pos = world_pos_texture.sample(s, uv);
    float3 viewDir = normalize(globalData->CameraPositionAndViewWidth.xyz - world_pos.xyz);

    S.BaseColor = albedo.xyz;
	S.Metallic = 0.f;
	S.Normal = normal_depth.xyz;
	S.PerceptualRoughness = 0.f;
	S.EmissiveColor = float3(0.f);
	S.EmissiveIntensity = 1.f;
	S.AmbientOcclusion = 1.f;

    for(uint i = 0; i < globalData->NumDirectionalLights; i++)
    {
        DirectionalLightParameters light = light_params[i];

        float3 lightDirection = normalize(light.DirectionAndIntensity.xyz);

        float4 shadow_map_hpos = light.LightMVP * float4(world_pos.xyz, 1.f);

        shadow_map_hpos.xyz = shadow_map_hpos.xyz / shadow_map_hpos.w;
        shadow_map_hpos.xy = (shadow_map_hpos.xy + 1.f) * 0.5f;
        shadow_map_hpos.y = 1.f - shadow_map_hpos.y;

        float shadow_depth = shadow_map_hpos.z;
        float2 shadow_map_uv = shadow_map_hpos.xy;
        float3 shadow_weight = 1.f;

        if(shadow_map_uv.x >= 0.f && shadow_map_uv.x <= 1.f && shadow_map_uv.y >= 0.f && shadow_map_uv.y <= 1.f)
        {
            float3 lightDir = -lightDirection;
            float NdotL = clamp(dot(normalize(S.Normal), lightDir), 0.f, 1.f);

            float bias = max(0.0005f * (1.f - NdotL), 0.0001f);

            float shadow_map_depth = shadow_mapping.sample(s, shadow_map_uv, i).r;
            if(shadow_depth > shadow_map_depth + bias)
            {
                shadow_weight = 0.f;
            }
        }

        if(abs(lightDirection.z - 1.f) < 0.0001f)
        {
            lightDirection = globalData->CameraDirectionAndViewHeight.xyz;
        }

        float3 light_contrib = CalculateLighting(S, -lightDirection, -viewDir, light.Color.xyz * light.DirectionAndIntensity.w);
        light_contrib = select(light_contrib, float3(0.f), isnan(light_contrib) || isinf(light_contrib));
        color += 0.1f * light_contrib * shadow_weight;
    }

    color = select(color, float3(0.f), isnan(color) || isinf(color));
    color = clamp(color, 0.f, 1.f);
    return float4(color, 1.f);
}