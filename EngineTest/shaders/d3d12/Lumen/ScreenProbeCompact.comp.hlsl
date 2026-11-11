static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer CompactConstantsBlock : register(b5, space0)
{
    uint4 compactConstants_constants : packoffset(c0);
};

ByteAddressBuffer probePositions : register(t0, space0);
RWByteAddressBuffer slotToCompact : register(u2, space0);
RWByteAddressBuffer compactStats : register(u4, space0);
RWByteAddressBuffer compactList : register(u3, space0);
ByteAddressBuffer probeNormals : register(t1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    uint totalSlots = compactConstants_constants.x;
    if (global_id >= totalSlots)
    {
        return;
    }
    float4 pos = asfloat(probePositions.Load4(global_id * 16 + 0));
    if (pos.w <= 0.0f)
    {
        slotToCompact.Store(global_id * 4 + 0, 4294967295u);
        return;
    }
    uint capacity = compactConstants_constants.y;
    uint _71;
    compactStats.InterlockedAdd(0, 1u, _71);
    uint denseId = _71;
    if (denseId >= capacity)
    {
        slotToCompact.Store(global_id * 4 + 0, 4294967295u);
        return;
    }
    slotToCompact.Store(global_id * 4 + 0, denseId);
    compactList.Store(denseId * 4 + 0, global_id);
    float tier = asfloat(probeNormals.Load(global_id * 16 + 12));
    if (tier <= 16.5f)
    {
        uint _106;
        compactStats.InterlockedAdd(4, 1u, _106);
    }
    else
    {
        if (tier <= 32.5f)
        {
            uint _115;
            compactStats.InterlockedAdd(8, 1u, _115);
        }
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
