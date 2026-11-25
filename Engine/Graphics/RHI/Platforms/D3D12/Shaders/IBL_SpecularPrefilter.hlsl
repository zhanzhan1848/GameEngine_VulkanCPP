// Vulkan reference: IBL_SpecularPrefilter.comp (main).
cbuffer PrefilterParams : register(b3, space0)
{
    uint params_faceSize : packoffset(c0);
    uint params_pad0 : packoffset(c0.y);
    float params_roughness : packoffset(c0.z);
    float params_srcResolution : packoffset(c0.w);
};

TextureCube<float4> envMap : register(t0, space0);
SamplerState smp : register(s2, space0);
RWTexture2DArray<float4> outputCube : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float2 hammersley(uint i)
{
    return float2(float(i) / 1024.0f, float(reversebits(i)) * 2.3283064e-10f);
}

float3 importanceSampleGGXTangent(float2 xi, float roughness)
{
    float a = roughness * roughness;
    float phi = 6.2831855f * xi.x;
    float cosTheta = sqrt(clamp((1.0f - xi.y) / (1.0f + (((a * a) - 1.0f) * xi.y)), 0.0f, 1.0f));
    float sinTheta = sqrt(max(1.0f - (cosTheta * cosTheta), 0.0f));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float distributionGGX(float3 n, float3 h, float roughness)
{
    float a2 = roughness * roughness;
    float NdotH = max(dot(n, h), 0.0f);
    float d = ((NdotH * NdotH) * (a2 - 1.0f)) + 1.0f;
    return a2 / (((3.1415927f * d) * d) + 0.0001f);
}

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    uint3 gid = input.dispatchThreadID;
    if ((((gid.x >= params_faceSize) || (gid.y >= params_faceSize)) || (gid.z >= 6u)))
    {
        return;
    }
    float2 uv = (float2(gid.xy) + 0.5f.xx) / float(params_faceSize).xx;
    float u = (2.0f * uv.x) - 1.0f;
    float v = -((2.0f * uv.y) - 1.0f);
    float3 N;
    switch (gid.z)
    {
        case 0u:
        {
            N = float3(1.0f, v, -u);
            break;
        }
        case 1u:
        {
            N = float3(-1.0f, v, u);
            break;
        }
        case 2u:
        {
            N = float3(u, 1.0f, -v);
            break;
        }
        case 3u:
        {
            N = float3(u, -1.0f, v);
            break;
        }
        case 4u:
        {
            N = float3(u, v, 1.0f);
            break;
        }
        default:
        {
            N = float3(-u, v, -1.0f);
            break;
        }
    }
    N = normalize(N);
    float3 V = N;
    bool3 _232 = (abs(N.z) < 0.999f).xxx;
    float3 up = float3(_232.x ? float3(0.0f, 0.0f, 1.0f).x : float3(1.0f, 0.0f, 0.0f).x, _232.y ? float3(0.0f, 0.0f, 1.0f).y : float3(1.0f, 0.0f, 0.0f).y, _232.z ? float3(0.0f, 0.0f, 1.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);
    float roughness = clamp(params_roughness, 0.0f, 1.0f);
    float totalWeight = 0.0f;
    float3 prefilteredColor = 0.0f.xxx;
    float _350;
    for (uint i = 0u; i < 1024u; i++)
    {
        float2 param_1 = hammersley(i);
        float3 h = importanceSampleGGXTangent(param_1, roughness);
        float3 H = normalize(((tangentX * h.x) + (tangentY * h.y)) + (N * h.z));
        float3 L = normalize((H * (2.0f * dot(V, H))) - V);
        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            float3 param_3 = N;
            float3 param_4 = H;
            float d = distributionGGX(param_3, param_4, roughness);
            float NdotH = max(dot(N, H), 0.0f);
            float HdotV = max(dot(H, V), 0.0f);
            float pdf = (d * NdotH) / ((4.0f * HdotV) + 0.0001f);
            float saTexel = 12.566371f / ((6.0f * params_srcResolution) * params_srcResolution);
            float saSample = 1.0f / ((1024.0f * pdf) + 0.0001f);
            if (roughness > 0.0f)
            {
                _350 = 0.5f * log2(saSample / saTexel);
            }
            else
            {
                _350 = 0.0f;
            }
            float mipLevel = _350;
            prefilteredColor += (envMap.SampleLevel(smp, L, mipLevel).xyz * NdotL);
            totalWeight += NdotL;
        }
    }
    outputCube[int3(gid)] = float4(prefilteredColor / totalWeight.xxx, 1.0f);
}
