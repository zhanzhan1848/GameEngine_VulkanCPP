/**
 * @file SurfaceCacheLightEval.metal
 * @brief Merged per-card texel lighting (LightCull + LightEval in one pass)
 *
 * Dispatch: 1D, total_texels = sum(card.resolution^2) threads.
 * ThreadGroupSize: (256, 1, 1)
 *
 * Thread mapping:
 *   global_tid → binary search card_dispatch[] → card_idx
 *   → local_texel = tid - card.texel_offset
 *   → atlas_uv = card.atlas_offset + (local_x, local_y)
 *
 * Inline per-texel light culling replaces the old separate LightCull pass.
 */

#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

kernel void surfaceCacheLightEval(
    uint global_tid [[thread_position_in_grid]],

    texture2d<float, access::read>  albedo_atlas    [[texture(0)]],
    texture2d<float, access::read>  normal_atlas    [[texture(1)]],
    texture2d<float, access::read>  emissive_atlas  [[texture(2)]],
    texture2d<float, access::write> lighting_out    [[texture(3)]],

    constant FlattenedLightingParams& params         [[buffer(1)]],
    constant SurfaceCacheCard*        cards          [[buffer(2)]],
    constant LightInfo*               lights         [[buffer(3)]],
    constant CardDispatchInfo*        card_dispatch  [[buffer(4)]])
{
    if (global_tid >= params.total_texels) return;

    // Binary search: find card whose [texel_offset, texel_offset+texel_count)
    // contains global_tid. O(log N) where N = card_count (typically < 100).
    uint lo = 0;
    uint hi = params.card_count;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        if (card_dispatch[mid].texel_offset + card_dispatch[mid].texel_count <= global_tid)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= params.card_count) return;

    uint local_texel = global_tid - card_dispatch[lo].texel_offset;
    uint res = card_dispatch[lo].resolution;
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_uv = uint2(
        card_dispatch[lo].atlas_offset_x + local_x,
        card_dispatch[lo].atlas_offset_y + local_y);

    // Read material data
    float4 albedo = albedo_atlas.read(atlas_uv);
    if (albedo.a < 0.01) return;

    float2 enc_n = normal_atlas.read(atlas_uv).rg;
    float3 normal = octDecode(enc_n);
    float3 emissive = emissive_atlas.read(atlas_uv).rgb;

    // Inline per-texel lighting (replaces separate LightCull pass)
    float3 direct_light = float3(0.0);
    uint lightCount = min(params.light_count, 256u);

    for (uint li = 0; li < lightCount; ++li) {
        float3 L;
        float attenuation;
        uint lightType = uint(lights[li].direction.w);

        if (lightType == 1) {
            // Directional light: always visible
            L = -normalize(lights[li].direction.xyz);
            attenuation = 1.0;
        } else {
            // Point/spot light: per-texel distance check
            float3 world_pos = cardTexelToWorld(atlas_uv, 0.0, cards[lo]);
            float3 to_light = lights[li].position.xyz - world_pos;
            float dist = length(to_light);
            float radius = lights[li].position.w;

            if (dist > radius) continue;

            L = to_light / max(dist, 0.001);
            float r2 = radius * radius;
            attenuation = max(1.0 - (dist * dist) / r2, 0.0);
        }

        float NdotL = max(dot(normal, L), 0.0);
        direct_light += lights[li].color.xyz * NdotL * attenuation;
    }

    float3 result = direct_light * albedo.rgb + emissive;
    lighting_out.write(float4(result, 1.0), atlas_uv);
}
