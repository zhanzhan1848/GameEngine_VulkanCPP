ByteAddressBuffer _115 : register(t1, space0);

static float3 vNormal;
static float vMode;
static uint vProbe;
static float4 outColor;
static float vState;
static float3 vWorld;
static float4 vClip;

struct SPIRV_Cross_Input
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
    nointerpolation float vState : TEXCOORD4;
    nointerpolation float vMode : TEXCOORD5;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float2 probeOctEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 uv = d.xy / l1norm.xx;
    if (d.z < 0.0f)
    {
        float2 s = lerp((-1.0f).xx, 1.0f.xx, float2(bool2(uv.x >= 0.0f.xx.x, uv.y >= 0.0f.xx.y)));
        uv = (1.0f.xx - abs(uv.yx)) * s;
    }
    return (uv * 0.5f) + 0.5f.xx;
}

void frag_main()
{
    float3 octN = normalize(vNormal);
    if (vMode > 1.5f)
    {
        float3 param = octN;
        float2 uv = probeOctEncode(param);
        uint2 texel = uint2(clamp(uint(uv.x * 8.0f), 0u, 7u), clamp(uint(uv.y * 8.0f), 0u, 7u));
        uint t = (texel.y * 8u) + texel.x;
        float meanD = asfloat(_115.Load(((vProbe * 128u) + t) * 4 + 0));
        float varD = asfloat(_115.Load((((vProbe * 128u) + 64u) + t) * 4 + 0));
        if (vMode < 2.5f)
        {
            float conf = 1.0f / (1.0f + varD);
            float3 heat = lerp(float3(0.85000002384185791015625f, 0.20000000298023223876953125f, 0.1500000059604644775390625f), float3(0.20000000298023223876953125f, 0.85000002384185791015625f, 0.300000011920928955078125f), clamp(conf, 0.0f, 1.0f).xxx);
            outColor = float4(heat, 1.0f);
        }
        else
        {
            float rel = clamp(varD / ((meanD * meanD) + 0.001000000047497451305389404296875f), 0.0f, 1.0f);
            float3 heat_1 = lerp(float3(0.20000000298023223876953125f, 0.449999988079071044921875f, 0.85000002384185791015625f), float3(0.949999988079071044921875f, 0.800000011920928955078125f, 0.20000000298023223876953125f), rel.xxx);
            outColor = float4(heat_1, 1.0f);
        }
        return;
    }
    if (vState < 1.5f)
    {
        float3 stateColor;
        if (vState < 0.125f)
        {
            stateColor = float3(0.449999988079071044921875f, 0.449999988079071044921875f, 0.4799999892711639404296875f);
        }
        else
        {
            if (vState < 0.625f)
            {
                stateColor = float3(0.89999997615814208984375f, 0.1500000059604644775390625f, 0.100000001490116119384765625f);
            }
            else
            {
                if (vState < 0.875f)
                {
                    stateColor = float3(0.949999988079071044921875f, 0.800000011920928955078125f, 0.100000001490116119384765625f);
                }
                else
                {
                    stateColor = float3(0.1500000059604644775390625f, 0.85000002384185791015625f, 0.25f);
                }
            }
        }
        float3 N = normalize(vNormal);
        float ndl = 0.5f + (0.5f * dot(N, float3(0.4239991605281829833984375f, 0.847998321056365966796875f, 0.317999362945556640625f)));
        outColor = float4(stateColor * (0.3499999940395355224609375f + (0.64999997615814208984375f * ndl)), 1.0f);
        return;
    }
    float h = frac(float(vProbe) * 0.61803400516510009765625f);
    float3 hue = clamp(float3((1.5f - abs((h * 6.0f) - 3.0f)) - 1.0f, (1.5f - abs((h * 6.0f) - 1.0f)) - 1.0f, (1.5f - abs((h * 6.0f) - 5.0f)) - 1.0f), 0.25f.xxx, 1.0f.xxx);
    float3 N_1 = normalize(vNormal);
    float ndl_1 = 0.5f + (0.5f * dot(N_1, float3(0.4239991605281829833984375f, 0.847998321056365966796875f, 0.317999362945556640625f)));
    float shade = 0.3499999940395355224609375f + (0.64999997615814208984375f * ndl_1);
    outColor = float4(hue * shade, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    vNormal = stage_input.vNormal;
    vMode = stage_input.vMode;
    vProbe = stage_input.vProbe;
    vState = stage_input.vState;
    vWorld = stage_input.vWorld;
    vClip = stage_input.vClip;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
