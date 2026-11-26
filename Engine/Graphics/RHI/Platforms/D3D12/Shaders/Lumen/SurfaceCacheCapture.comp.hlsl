// Vulkan reference: Lumen/SurfaceCacheCapture.comp (surface_cache_capture).
struct SurfaceCacheCard
{
    float4 center;
    float4 extent;
    uint axis_direction;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint mesh_instance_id;
    uint static_mesh_slot;
    uint flags;
    uint page_card;
};

ByteAddressBuffer gridOffsets : register(t8, space0);
ByteAddressBuffer cards : register(t7, space0);
ByteAddressBuffer gridCards : register(t9, space0);
cbuffer CaptureParams : register(b6, space0)
{
    row_major float4x4 cp_invViewProjection : packoffset(c0);
    uint cp_card_count : packoffset(c4);
    uint cp_render_width : packoffset(c4.y);
    uint cp_render_height : packoffset(c4.z);
    uint cp_atlas_size : packoffset(c4.w);
};

RWByteAddressBuffer minDepth : register(u10, space0);
cbuffer CaptureEmissive : register(b12, space0)
{
    float4 ce_emissive_rgba : packoffset(c0);
};

ByteAddressBuffer instanceEmissive : register(t13, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u3, space0);
RWTexture2D<float2> normal_atlas : register(u4, space0);
RWTexture2D<float> depth_atlas : register(u5, space0);
RWTexture2D<float4> emissive_atlas : register(u11, space0);
Texture2D<float4> gbuffer_depth : register(t0, space0);
Texture2D<float4> gbuffer_albedo : register(t1, space0);
Texture2D<float4> gbuffer_normal : register(t2, space0);

// Grid offsets begin with a 16-word header, followed by cell prefix sums.
bool projectToCard(float3 worldPos, SurfaceCacheCard card, out uint2 texel, out float axisDepth)
{
    float3 local = worldPos - card.center.xyz;
    uint axis = card.axis_direction & 0xffu;
    float signZ = ((card.axis_direction >> 8) & 0xffu) ? 1.0 : -1.0;
    float2 uv;
    if (axis == 0) {
        axisDepth = signZ * local.x;
        uv = local.yz / (card.extent.yz * 2.0) + 0.5;
    } else if (axis == 1) {
        axisDepth = signZ * local.y;
        uv = local.xz / (card.extent.xz * 2.0) + 0.5;
    } else {
        axisDepth = signZ * local.z;
        uv = local.xy / (card.extent.xy * 2.0) + 0.5;
    }
    texel = 0;
    if (axisDepth < 0 || any(uv < 0) || any(uv >= 1)) return false;
    texel = uint2(float2(card.atlas_offset_x, card.atlas_offset_y) + uv * card.resolution);
    return true;
}

void scatter(uint2 pixel, bool writePhase)
{
    if (any(pixel >= uint2(cp_render_width, cp_render_height))) return;
    float depth = gbuffer_depth.Load(int3(pixel, 0)).r;
    if (depth >= 1) return;
    float2 uv = (float2(pixel) + 0.5) / float2(cp_render_width, cp_render_height);
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 world = mul(float4(ndc, depth, 1), cp_invViewProjection);
    float3 worldPos = world.xyz / world.w;

    float3 gridMin = asfloat(gridOffsets.Load3(0));
    float cellSize = asfloat(gridOffsets.Load(28));
    uint3 dims = gridOffsets.Load3(32);
    float3 gridPos = (worldPos - gridMin) / cellSize;
    uint3 cell = uint3(gridPos);
    if (any(gridPos < 0) || any(cell >= dims)) return;
    uint cellIndex = cell.x + dims.x * (cell.y + dims.y * cell.z);
    uint2 range = gridOffsets.Load2((16u + cellIndex) * 4u);

    float3 albedo = 0;
    float2 encodedNormal = 0;
    if (writePhase) {
        albedo = gbuffer_albedo.Load(int3(pixel, 0)).rgb;
        float3 n = normalize(gbuffer_normal.Load(int3(pixel, 0)).xyz * 2 - 1);
        encodedNormal = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
        if (n.z < 0)
            encodedNormal = (1 - abs(encodedNormal.yx)) * lerp(-1.0, 1.0, step(0.0, encodedNormal));
        encodedNormal = encodedNormal * 0.5 + 0.5;
    }
    for (uint i = range.x; i < range.y; ++i) {
        uint cardIndex = gridCards.Load(i * 4u);
        SurfaceCacheCard card = cards.Load<SurfaceCacheCard>(cardIndex * 64u);
        if ((card.flags & 1u) != 0) continue;
        uint2 texel;
        float axisDepth;
        if (!projectToCard(worldPos, card, texel, axisDepth)) continue;
        uint offset = (texel.y * cp_atlas_size + texel.x) * 4u;
        if (!writePhase) {
            uint previous;
            minDepth.InterlockedMin(offset, asuint(axisDepth), previous);
        } else if (minDepth.Load(offset) == asuint(axisDepth)) {
            // Bit 7 marks an opaque valid texel in the packed transport contract.
            albedo_atlas[texel] = float4(albedo, 128.0 / 255.0);
            normal_atlas[texel] = encodedNormal;
            depth_atlas[texel] = axisDepth;
            float4 emissive = instanceEmissive.Load<float4>(min(card.mesh_instance_id, 1023u) * 16u);
            if (!(emissive.a > 0.5)) emissive = ce_emissive_rgba;
            if (emissive.a > 0.5) emissive_atlas[texel] = float4(emissive.rgb, 1);
        }
    }
}

[numthreads(8, 8, 1)]
void clearDepth(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= cp_atlas_size)) return;
    minDepth.Store((id.y * cp_atlas_size + id.x) * 4u, 0xffffffffu);
}

[numthreads(8, 8, 1)]
void captureDepth(uint3 id : SV_DispatchThreadID) { scatter(id.xy, false); }

[numthreads(8, 8, 1)]
void writeAtlas(uint3 id : SV_DispatchThreadID) { scatter(id.xy, true); }
