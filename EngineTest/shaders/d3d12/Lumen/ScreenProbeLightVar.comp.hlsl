static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

ByteAddressBuffer prevSH : register(t0, space0);
cbuffer LightVarConstantsBlock : register(b4, space0)
{
    uint4 lightVarConstants_constants[2] : packoffset(c0);
};

RWByteAddressBuffer lightVarState : register(u1, space0);
RWByteAddressBuffer lightVarStats : register(u3, space0);
RWByteAddressBuffer lightVarMask : register(u2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float tapLuminance(uint tileIdx, uint gridW, uint gridH)
{
    uint totalTiles = gridW * gridH;
    if (tileIdx >= totalTiles)
    {
        return -1.0f;
    }
    uint slot = tileIdx * 4u;
    float4 sh0 = asfloat(prevSH.Load4(((slot * 4u) + 0u) * 16 + 0));
    if (sh0.w <= 0.0f)
    {
        return -1.0f;
    }
    return dot(sh0.xyz, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    uint totalTiles = lightVarConstants_constants[0].x;
    if (global_id >= totalTiles)
    {
        return;
    }
    uint gridW = lightVarConstants_constants[0].y;
    uint gridH = lightVarConstants_constants[0].z;
    uint cap = lightVarConstants_constants[0].w;
    float hi = asfloat(lightVarConstants_constants[1].x);
    float lo = asfloat(lightVarConstants_constants[1].y);
    uint gx = global_id % gridW;
    uint gy = global_id / gridW;
    uint param = global_id;
    uint param_1 = gridW;
    uint param_2 = gridH;
    float l[5];
    l[0] = tapLuminance(param, param_1, param_2);
    float _130;
    if (gx > 0u)
    {
        uint param_3 = global_id - 1u;
        uint param_4 = gridW;
        uint param_5 = gridH;
        _130 = tapLuminance(param_3, param_4, param_5);
    }
    else
    {
        _130 = -1.0f;
    }
    l[1] = _130;
    float _149;
    if ((gx + 1u) < gridW)
    {
        uint param_6 = global_id + 1u;
        uint param_7 = gridW;
        uint param_8 = gridH;
        _149 = tapLuminance(param_6, param_7, param_8);
    }
    else
    {
        _149 = -1.0f;
    }
    l[2] = _149;
    float _166;
    if (gy > 0u)
    {
        uint param_9 = global_id - gridW;
        uint param_10 = gridW;
        uint param_11 = gridH;
        _166 = tapLuminance(param_9, param_10, param_11);
    }
    else
    {
        _166 = -1.0f;
    }
    l[3] = _166;
    float _186;
    if ((gy + 1u) < gridH)
    {
        uint param_12 = global_id + gridW;
        uint param_13 = gridW;
        uint param_14 = gridH;
        _186 = tapLuminance(param_12, param_13, param_14);
    }
    else
    {
        _186 = -1.0f;
    }
    l[4] = _186;
    float sum = 0.0f;
    float sumSq = 0.0f;
    float n = 0.0f;
    for (int i = 0; i < 5; i++)
    {
        if (l[i] < 0.0f)
        {
            continue;
        }
        sum += l[i];
        sumSq += (l[i] * l[i]);
        n += 1.0f;
    }
    float variance = 0.0f;
    if (n > 1.5f)
    {
        float mean = sum / n;
        variance = max((sumSq / n) - (mean * mean), 0.0f);
    }
    uint state = lightVarState.Load(global_id * 4 + 0);
    float emaPrev = float(state & 255u) / 4096.0f;
    float ema = lerp(emaPrev, variance, 0.300000011920928955078125f);
    bool isActive = ((state >> 16u) & 255u) != 0u;
    uint below = (state >> 8u) & 255u;
    bool _295 = (!isActive) && (ema > hi);
    bool _301;
    if (_295)
    {
        _301 = l[0] >= 0.0f;
    }
    else
    {
        _301 = _295;
    }
    if (_301)
    {
        uint _310;
        lightVarStats.InterlockedAdd(0, 1u, _310);
        uint prev = _310;
        if (prev < cap)
        {
            isActive = true;
            below = 0u;
        }
        else
        {
            uint _320;
            lightVarStats.InterlockedAdd(0, 4294967295u, _320);
        }
    }
    else
    {
        if (isActive)
        {
            if (ema < lo)
            {
                below++;
                if (below >= 30u)
                {
                    isActive = false;
                    below = 0u;
                    uint _339;
                    lightVarStats.InterlockedAdd(0, 4294967295u, _339);
                }
            }
            else
            {
                below = 0u;
            }
        }
    }
    uint emaQ = uint(clamp(ema, 0.0f, 0.0625f) * 4096.0f);
    lightVarState.Store(global_id * 4 + 0, ((emaQ & 255u) | ((below & 255u) << 8u)) | (uint(isActive) << 16u));
    lightVarMask.Store(global_id * 4 + 0, uint(isActive));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
