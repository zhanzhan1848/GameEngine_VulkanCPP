// Vulkan reference: Debug/ProbeSphere.frag (main).
ByteAddressBuffer momentsSSBO : register(t1, space0);

struct StageInput
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
    nointerpolation float vState : TEXCOORD4;
    nointerpolation float vMode : TEXCOORD5;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float2 probeOctEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 uv = d.xy / l1norm;
    if (d.z < 0.0) {
        float2 s = lerp((float2)(-1.0), (float2)(1.0), (float2)(((uv.xy) >= ((float2)(0.0)))));
        uv = (1.0 - abs(uv.yx)) * s;
    }
    return uv * 0.5 + 0.5;
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 octN = normalize(input.vNormal);
    if (input.vMode > 1.5f)
    {
        float2 uv = probeOctEncode(octN);
        uint2 texel = uint2(clamp(uint(uv.x * 8.0f), 0u, 7u), clamp(uint(uv.y * 8.0f), 0u, 7u));
        uint t = (texel.y * 8u) + texel.x;
        float meanD = asfloat(momentsSSBO.Load(((input.vProbe * 128u) + t) * 4));
        float varD = asfloat(momentsSSBO.Load((((input.vProbe * 128u) + 64u) + t) * 4));
        if (input.vMode < 2.5f)
        {
            float conf = 1.0f / (1.0f + varD);
            float3 heat = lerp(float3(0.85f, 0.2f, 0.15f), float3(0.2f, 0.85f, 0.3f), clamp(conf, 0.0f, 1.0f).xxx);
            output.outColor = float4(heat, 1.0f);
        }
        else
        {
            float rel = clamp(varD / ((meanD * meanD) + 0.001f), 0.0f, 1.0f);
            float3 heat_1 = lerp(float3(0.2f, 0.45f, 0.85f), float3(0.95f, 0.8f, 0.2f), rel.xxx);
            output.outColor = float4(heat_1, 1.0f);
        }
        return output;
    }
    if (input.vState < 1.5f)
    {
        float3 stateColor;
        if (input.vState < 0.125f)
        {
            stateColor = float3(0.45f, 0.45f, 0.48f);
        }
        else
        {
            if (input.vState < 0.625f)
            {
                stateColor = float3(0.9f, 0.15f, 0.1f);
            }
            else
            {
                if (input.vState < 0.875f)
                {
                    stateColor = float3(0.95f, 0.8f, 0.1f);
                }
                else
                {
                    stateColor = float3(0.15f, 0.85f, 0.25f);
                }
            }
        }
        float3 N = normalize(input.vNormal);
        float ndl = 0.5f + (0.5f * dot(N, float3(0.42399916f, 0.8479983f, 0.31799936f)));
        output.outColor = float4(stateColor * (0.35f + (0.65f * ndl)), 1.0f);
        return output;
    }
    float h = frac(float(input.vProbe) * 0.618034f);
    float3 hue = clamp(float3((1.5f - abs((h * 6.0f) - 3.0f)) - 1.0f, (1.5f - abs((h * 6.0f) - 1.0f)) - 1.0f, (1.5f - abs((h * 6.0f) - 5.0f)) - 1.0f), 0.25f.xxx, 1.0f.xxx);
    float3 N_1 = normalize(input.vNormal);
    float ndl_1 = 0.5f + (0.5f * dot(N_1, float3(0.42399916f, 0.8479983f, 0.31799936f)));
    float shade = 0.35f + (0.65f * ndl_1);
    output.outColor = float4(hue * shade, 1.0f);
    return output;
}
