static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer InjectParams : register(b4, space0)
{
    row_major float4x4 params_invViewProjection : packoffset(c0);
    float4 params_cameraPos : packoffset(c4);
    float4 params_gridMin : packoffset(c5);
    float4 params_cellSize : packoffset(c6);
    float4 params_gridDims : packoffset(c7);
    float4 params_screen : packoffset(c8);
};

Texture2D<float4> depthTexture : register(t1, space0);
Texture2D<float4> directDiffuse : register(t0, space0);
Texture2D<float4> gbufferNormal : register(t2, space0);
Texture2D<float4> gbufferMaterial : register(t3, space0);
RWTexture3D<float4> outSH : register(u5, space0);

static uint3 gl_WorkGroupID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared float4 sR[64];
groupshared float4 sG[64];
groupshared float4 sB[64];

float3 unproject(float2 ndcXY, float depth)
{
    float4 h = mul(float4(ndcXY, depth, 1.0f), params_invViewProjection);
    return h.xyz / max(h.w, 9.9999999747524270787835121154785e-07f).xxx;
}

void comp_main()
{
    int3 probe = int3(gl_WorkGroupID);
    float3 p = params_gridMin.xyz + ((float3(gl_WorkGroupID) + 0.5f.xxx) * params_cellSize.xyz);
    int W = int(params_screen.x);
    int H = int(params_screen.y);
    int S = int(params_screen.z);
    int nx = ((W + S) - 1) / S;
    int ny = ((H + S) - 1) / S;
    float4 accR = 0.0f.xxxx;
    float4 accG = 0.0f.xxxx;
    float4 accB = 0.0f.xxxx;
    int _115 = int(gl_LocalInvocationIndex);
    for (int i = _115; i < (nx * ny); i += 64)
    {
        int2 px = min(int2(((i % nx) * S) + (S / 2), ((i / nx) * S) + (S / 2)), int2(W - 1, H - 1));
        float depth = depthTexture.Load(int3(px, 0)).x;
        if (depth >= 1.0f)
        {
            continue;
        }
        float3 L = directDiffuse.Load(int3(px, 0)).xyz;
        if (((L.x + L.y) + L.z) <= 0.0f)
        {
            continue;
        }
        float2 uv = (float2(px) + 0.5f.xx) / float2(float(W), float(H));
        float2 ndc = (uv * 2.0f) - 1.0f.xx;
        float2 param = ndc;
        float param_1 = depth;
        float3 x = unproject(param, param_1);
        float2 param_2 = ndc + float2((2.0f * float(S)) / float(W), 0.0f);
        float param_3 = depth;
        float3 xr = unproject(param_2, param_3);
        float2 param_4 = ndc + float2(0.0f, (2.0f * float(S)) / float(H));
        float param_5 = depth;
        float3 xu = unproject(param_4, param_5);
        float2 nxy = gbufferNormal.Load(int3(px, 0)).xy;
        float zSign = (gbufferMaterial.Load(int3(px, 0)).w * 2.0f) - 1.0f;
        float3 nj = normalize(float3(nxy, zSign * sqrt(max(0.0f, 1.0f - dot(nxy, nxy)))));
        float area = (length(xr - x) * length(xu - x)) / max(abs(dot(nj, normalize(params_cameraPos.xyz - x))), 0.20000000298023223876953125f);
        float3 d = x - p;
        float r2 = max(dot(d, d), 9.9999999747524270787835121154785e-07f);
        float3 w = d * rsqrt(r2);
        float cosj = dot(nj, -w);
        if (cosj <= 0.0f)
        {
            continue;
        }
        float dOmega = (area * cosj) / (r2 + (area * 0.3183098733425140380859375f));
        float4 Y = float4(0.2820949852466583251953125f, 0.48860299587249755859375f * w.y, 0.48860299587249755859375f * w.z, 0.48860299587249755859375f * w.x);
        accR += (Y * (L.x * dOmega));
        accG += (Y * (L.y * dOmega));
        accB += (Y * (L.z * dOmega));
    }
    sR[gl_LocalInvocationIndex] = accR;
    sG[gl_LocalInvocationIndex] = accG;
    sB[gl_LocalInvocationIndex] = accB;
    GroupMemoryBarrierWithGroupSync();
    for (int s = 32; s > 0; s = s >> 1)
    {
        if (int(gl_LocalInvocationIndex) < s)
        {
            sR[gl_LocalInvocationIndex] += sR[gl_LocalInvocationIndex + uint(s)];
            sG[gl_LocalInvocationIndex] += sG[gl_LocalInvocationIndex + uint(s)];
            sB[gl_LocalInvocationIndex] += sB[gl_LocalInvocationIndex + uint(s)];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gl_LocalInvocationIndex == 0u)
    {
        int Nz = int(params_gridDims.z);
        outSH[int3(probe.xy, probe.z)] = sR[0];
        outSH[int3(probe.xy, probe.z + Nz)] = sG[0];
        outSH[int3(probe.xy, probe.z + (2 * Nz))] = sB[0];
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
