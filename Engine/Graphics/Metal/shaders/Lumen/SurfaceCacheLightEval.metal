#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct LightEvalParams {
    SurfaceCacheParams sc_params;
    uint tiles_x;
    uint tile_size;
    uint light_count;
};

kernel void surfaceCacheLightEval(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read>  albedo_atlas    [[texture(0)]],
    texture2d<float, access::read>  normal_atlas    [[texture(1)]],
    texture2d<float, access::read>  emissive_atlas  [[texture(2)]],
    depth2d<float, access::sample>  shadow_map      [[texture(3)]],
    texture2d<float, access::write> lighting_out    [[texture(4)]],
    device GlobalShaderData&        gd              [[buffer(0)]],
    constant LightEvalParams&       params          [[buffer(1)]],
    constant SurfaceCacheCard*      cards           [[buffer(2)]],
    constant LightInfo*             lights          [[buffer(3)]],
    device const uint4*             tile_lights     [[buffer(4)]])
{
    if (global_id.x >= params.sc_params.atlas_size || global_id.y >= params.sc_params.atlas_size) return;

    float4 albedo = albedo_atlas.read(global_id);
    if (albedo.a < 0.01) return;

    float2 enc_n = normal_atlas.read(global_id).rg;
    float3 normal = octDecode(enc_n);
    float3 emissive = emissive_atlas.read(global_id).rgb;

    uint tile_x = global_id.x / params.tile_size;
    uint tile_y = global_id.y / params.tile_size;
    uint tile_idx = tile_y * params.tiles_x + tile_x;
    uint4 assigned = tile_lights[tile_idx];

    float3 direct_light = float3(0.0);

    for (uint i = 0; i < 4; ++i) {
        uint light_idx;
        switch(i) {
            case 0: light_idx = assigned.x; break;
            case 1: light_idx = assigned.y; break;
            case 2: light_idx = assigned.z; break;
            default: light_idx = assigned.w; break;
        }
        if (light_idx == 0xFFFFFFFF) continue;

        float3 L;
        float attenuation;
        if (lights[light_idx].type == 1) {
            L = -normalize(lights[light_idx].direction);
            attenuation = 1.0;
        } else {
            // Point light: reconstruct world pos from card data
            float3 world_pos = cardTexelToWorld(global_id, 0.0, cards[0]);
            float3 to_light = lights[light_idx].position - world_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 0.001);
            float r2 = lights[light_idx].radius * lights[light_idx].radius;
            attenuation = max(1.0 - (dist * dist) / r2, 0.0);
        }

        float NdotL = max(dot(normal, L), 0.0);
        direct_light += lights[light_idx].color * NdotL * attenuation;
    }

    float3 result = direct_light * albedo.rgb + emissive;
    lighting_out.write(float4(result, 1.0), global_id);
}
