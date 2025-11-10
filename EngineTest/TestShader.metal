#include "Common.h"

#define ElementsTypeStaticNormal                0x01
#define ElementsTypeStaticNormalTexture         0x03
#define ElementsTypeStaticColor                 0x04
#define ElementsTypeSkeletal                    0x08
#define ElementsTypeSkeletalColor               0x0C  // 0x08 | 0x04
#define ElementsTypeSkeletalNormal              0x09  // 0x08 | 0x01
#define ElementsTypeSkeletalNormalColor         0x0D  // 0x08 | 0x01 | 0x04
#define ElementsTypeSkeletalNormalTexture       0x0B  // 0x08 | 0x03
#define ElementsTypeSkeletalNormalTextureColor  0x0F  // 0x08 | 0x03 | 0x04

struct Surface
{
	float3 BaseColor;
	float Metallic;
	float3 Normal;
	float PerceptualRoughness;
	float3 EmissiveColor;
	float EmissiveIntensity;
	float AmbientOcclusion;
};

struct VertexPosition
{
    device float3* positions;
};

struct VertexElement
{
    uint            ColorTSign;
    packed_ushort2         Normal;
    packed_ushort2         Tangent;
    packed_float2          UV;
};

struct VertexOut
{
    float4 HomogeneousPosition [[position]];
	float3 WorldPosition;	
	float3 WorldNormal;		
	float3 WorldTangent;			
	float2 UV;		
};

struct PixelOut
{
    float4 Color [[color(0)]];
    float4 Normal_Depth [[color(1)]];
    float4 Albedo [[color(2)]];
};

constant float InvIntervals = 2.f / ((1 << 16) - 1);

struct GlobalData
{
    device const GlobalShaderData* global_shader_data [[id(0)]];
    device const PerObjectData* per_object_data [[id(1)]];
    device const packed_float3* vertices [[id(2)]];
    device const VertexElement* elements [[id(3)]];
    device const uint* srv_indices [[id(4)]];
    device const DirectionalLightParameters* directional_light_params [[id(5)]];
};

struct StaticSamplerState
{
    sampler pointSampler [[id(0)]];
    sampler linearSampler [[id(1)]];
    sampler anisoSampler [[id(2)]];
};

Surface GetSurface(VertexOut psIn)
{
	Surface surface;
	surface.BaseColor = float3(1.f);
	surface.Metallic = 0.f;
	surface.Normal = normalize(psIn.WorldNormal);  // 修复：使用正确的法向量而不是标量值
	surface.PerceptualRoughness = 0.f;
	surface.EmissiveColor = float3(0.f);
	surface.EmissiveIntensity = 1.f;
	surface.AmbientOcclusion = 1.f;

#if TEXTURED_MTL
	surface.AmbientOcclusion = Sampler(0, LinearSampler, uv).r;
	surface.BaseColor = Sampler(1, LinearSampler, uv).rgb;
	surface.EmissiveColor = Sampler(2, LinearSampler, uv).rgb;
	float2 metalRough = Sampler(3, LinearSampler, uv).rg;  // 修复：语法错误，应该是.rg
	surface.Metallic = metalRough.r;
	surface.PerceptualRoughness = metalRough.g;
	surface.EmissiveIntensity = 1.f;
	float3 n = Sampler(4, LinearSampler, uv).rgb;
	surface.Normal = psIn.WorldNormal;
#endif

	return surface;
}

float LinearizeDepth(float depth)
{
    float nearClip = 0.1f;
    float farClip = 64.f;
    return (2.0 * nearClip * farClip) / (farClip + nearClip - depth * (farClip - nearClip));
}

float3 PhongBRDF(float3 N, float3 L, float3 V, float3 diffuseColor, float3 specularColor, float shininess)
{
	float3 color = diffuseColor;
	const float3 R = reflect(-L, N);
	const float VoR = max(dot(V, R), 0.f);
	color += pow(VoR, max(shininess, 1.f)) * specularColor;

	return color;
}

float3 CalculateLighting(Surface S, float3 L, float3 V, float3 lightColor)
{
    const float NoL = clamp(dot(S.Normal, L), 0.f, 1.f);
    // 确保PI不为零，避免除零错误
    const float invPI = 1.0f / max(PI, 1e-6f);
	return PhongBRDF(S.Normal, L, V, S.BaseColor, 1.f, (1 - S.PerceptualRoughness) * 100.f) * (NoL * invPI) * lightColor;
}

VertexOut vertex vertex_main(device const GlobalData* global_data [[buffer(0)]],
                             uint vertex_index [[vertex_id]])
{
    VertexOut vsOut;

    // if ELEMENT_TYPE == 3
    float4 pos = float4(global_data->vertices[vertex_index], 1.f);
    float4 worldPosition = global_data->per_object_data->World * pos;

    VertexElement element = global_data->elements[vertex_index];
    float2 nXY = float2(element.Normal) * InvIntervals - 1.f;
    uint signs = (element.ColorTSign >> 24) & 0xff;
    float nSign = float(signs & 0x02) - 1;
    float3 normal = normalize(float3(nXY.x, nXY.y, sqrt(clamp(1.f - dot(nXY, nXY), 0.f, 1.f)) * nSign));

    float2 tXY = float2(element.Tangent) * InvIntervals - 1.f;
    float tSign = float(signs & 0x01) - 1; // 假设切线符号在位 0
    float3 tangent = float3(tXY.x, tXY.y, sqrt(clamp(1.f - dot(tXY, tXY), 0.f, 1.f)) * tSign);

    vsOut.HomogeneousPosition = global_data->per_object_data->WorldViewProjection * worldPosition;
    vsOut.WorldPosition	  = worldPosition.xyz;
    vsOut.WorldNormal	  = (global_data->per_object_data->InvWorld * float4(normal, 0.f)).xyz;
    vsOut.WorldTangent	  = (global_data->per_object_data->World * float4(tangent, 0.f)).xyz;;
    vsOut.UV			  = element.UV;

    return vsOut;
}

PixelOut fragment fragment_main(VertexOut vsOut [[stage_in]],
                             device const GlobalData* global_data [[buffer(0)]],
                             device StaticSamplerState& static_sampler_state [[buffer(1)]],
                             texture2d_array<float, access::sample> texture_array [[texture(0)]]
                            )
{
    PixelOut psOut;
    float3 color = float3(0.f);
    float3 viewDir = normalize(global_data->global_shader_data->CameraPositionAndViewWidth.xyz - vsOut.WorldPosition);

    Surface S = GetSurface(vsOut);

    for(uint i = 0; i < 3; ++i)
	{
        // if(i != 0) continue;
		DirectionalLightParameters light = global_data->directional_light_params[i];

        float3 lightDirection = normalize(light.DirectionAndIntensity.xyz);
        
        // 计算阴影映射坐标
        float4 shadow_map_hpos = light.LightMVP * float4(vsOut.WorldPosition, 1.f);
        
        // 透视除法
        shadow_map_hpos.xyz = shadow_map_hpos.xyz / shadow_map_hpos.w;
        
        // Metal坐标系统：NDC空间已经是[-1,1]，直接转换到纹理空间[0,1]
        shadow_map_hpos.xy = shadow_map_hpos.xy * 0.5f + 0.5f;
        shadow_map_hpos.y = 1.0f - shadow_map_hpos.y; // Metal Y轴翻转
        
        // Metal 现在的深度计算矩阵的计算结果为 [0, 1]
        float shadow_depth = shadow_map_hpos.z;
        
        float2 shadow_map_uv = shadow_map_hpos.xy;
        float3 shadow_weight = 1.f;
        
        // 检查是否在阴影贴图范围内
        if(shadow_map_uv.x >= 0.f && shadow_map_uv.x <= 1.f && 
           shadow_map_uv.y >= 0.f && shadow_map_uv.y <= 1.f)
        {
            // 改进的bias计算，考虑Metal坐标系统
            float3 normal = normalize(vsOut.WorldNormal);
            float3 lightDir = -lightDirection;
            float NdotL = clamp(dot(normal, lightDir), 0.0f, 1.0f);
            
            // 基于斜率的动态bias，适配Metal深度精度
            float bias = max(0.0005f * (1.0f - NdotL), 0.0001f);
            
            // 从阴影贴图采样深度值
            float shadow_map_depth = texture_array.sample(static_sampler_state.linearSampler, shadow_map_uv, i).r;
            
            // 深度比较：当前片元深度 > 阴影贴图深度 + bias 时产生阴影
            if(shadow_depth > shadow_map_depth + bias)
            {
                shadow_weight = 0.2f; // 柔和阴影效果
            }
        }
        
		if(abs(lightDirection.z - 1.f) < 0.001f)
		{
			lightDirection = global_data->global_shader_data->CameraDirectionAndViewHeight.xyz;
		}
        
        float3 lightContribution = CalculateLighting(S, -lightDirection, -viewDir, light.Color.xyz * light.DirectionAndIntensity.w);
        
        // 检查并修复无效值
        lightContribution = select(lightContribution, float3(0.0f), isnan(lightContribution) || isinf(lightContribution));
        
        color += 0.1f * lightContribution * shadow_weight; // * shadow_weight
    }

    // 确保最终颜色值有效
    color = select(color, float3(0.0f), isnan(color) || isinf(color));
    color = clamp(color, 0.0f, 1.0f);
    // gamma
    // color = pow(color, float3(0.4545));

    // 输出颜色
    psOut.Color = float4(color, 1.f);
    psOut.Albedo = float4(S.BaseColor, 1.f);
    // 输出法线和深度
    float depth = vsOut.HomogeneousPosition.z;
    // 使用正确的线性深度计算函数
    float linearDepth = LinearizeDepth(depth);
    psOut.Normal_Depth = float4(normalize(vsOut.WorldNormal), linearDepth);
    
    return psOut;
}