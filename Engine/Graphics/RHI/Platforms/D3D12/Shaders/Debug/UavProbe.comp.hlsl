// Vulkan reference: Debug/UavProbe.comp (main).
Texture2D<float4> u_probe_src : register(t1, space0);
Texture2D<float4> u_probe_depth : register(t2, space0);
RWTexture2D<float4> u_probe : register(u0, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    float4 a = u_probe_src.Load(int3(int2(input.dispatchThreadID.xy), 0));
    float d = u_probe_depth.Load(int3(int2(input.dispatchThreadID.xy), 0)).x;
    u_probe[int2(input.dispatchThreadID.xy)] = float4(1.0f + (a.x * 0.25f), 0.5f + (a.y * 0.25f), 0.25f + (d * 0.5f), 1.0f);
}
