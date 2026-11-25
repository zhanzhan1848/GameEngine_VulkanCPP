// Vulkan reference: Lumen/SSGIHalfResDenoise.comp (main).
cbuffer HalfResDenoiseParams : register(b2, space0)
{
    uint params_width : packoffset(c0);
    uint params_height : packoffset(c0.y);
    float params_sigma : packoffset(c0.z);
    float params_pad0 : packoffset(c0.w);
    float params_pad1 : packoffset(c1);
    float params_pad2 : packoffset(c1.y);
    float params_pad3 : packoffset(c1.z);
    float params_pad4 : packoffset(c1.w);
};

Texture2D<float4> trace_input : register(t0, space0);
RWTexture2D<float4> trace_output : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;
    if (((gid.x >= params_width) || (gid.y >= params_height)))
    {
        return;
    }
    float invTwoSigmaSq = 1.0f / ((2.0f * params_sigma) * params_sigma);
    float3 filteredIrr = 0.0f.xxx;
    float filteredDist = 0.0f;
    float totalWeight = 0.0f;
    for (int dy = -2; dy <= 2; dy++)
    {
        for (int dx = -2; dx <= 2; dx++)
        {
            int2 pos = int2(gid) + int2(dx, dy);
            if (((((pos.x < 0) || (pos.y < 0)) || (pos.x >= int(params_width))) || (pos.y >= int(params_height))))
            {
                continue;
            }
            float4 s = trace_input.Load(int3(pos, 0));
            float spatial2 = float((dx * dx) + (dy * dy));
            float w = exp((-spatial2) * invTwoSigmaSq);
            filteredIrr += (s.xyz * w);
            filteredDist += (s.w * w);
            totalWeight += w;
        }
    }
    if (totalWeight > 0.001f)
    {
        trace_output[int2(gid)] = float4(filteredIrr / totalWeight.xxx, filteredDist / totalWeight);
    }
    else
    {
        trace_output[int2(gid)] = trace_input.Load(int3(int2(gid), 0));
    }
}
