#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

// Metal twin of SurfaceCacheIndirectReconstruct.comp (plan §三.A slice 1):
// replace the piecewise-constant tile splat with a CONTINUOUS reconstruction
// on the tile-center lattice. Per atlas texel: validated bilinear over the 4
// surrounding tile centers (same owning card, normal within ~45°, capture
// depth within 2×hit_band — see the GLSL twin for the full contract), then
// multiply by the RECEIVING texel's own albedo (incident/material separation).
// tile_grid = (rgb = incident radiance, a = validity); tile_aux =
// (rg = oct16 normal, b = capture depth, a = owning card id, half-exact).

struct IndirectResolveParams {
    SurfaceCacheParams sc_params;
    uint tile_size;
    uint rays_per_probe;
    float temporal_weight;
    uint frame_index;
    uint lookup_count;
    uint refresh_stride;
    uint refresh_phase;
    float hit_band;
    uint recon_enabled;
    uint card_count;
};

kernel void surfaceCacheIndirectReconstruct(
    uint2 global_id [[thread_position_in_grid]],
    constant IndirectResolveParams& params [[buffer(0)]],
    texture2d<float, access::read>  albedo_atlas [[texture(0)]],
    texture2d<float, access::read>  tile_grid    [[texture(1)]],
    texture2d<float, access::read>  tile_aux     [[texture(2)]],
    texture2d<float, access::write> indirect_out [[texture(3)]],
    texture2d<float, access::read>  depth_atlas  [[texture(4)]],
    texture2d<float, access::read>  normal_atlas [[texture(5)]])
{
    const uint2 texel = global_id;
    const uint A = params.sc_params.atlas_size;
    if (texel.x >= A || texel.y >= A) return;

    const uint T = params.tile_size;
    const uint tilesX = A / T;

    float3 albedo = albedo_atlas.read(texel).rgb;
    float2 ownEnc = normal_atlas.read(texel).rg;
    float3 ownN = octDecode(ownEnc);

    const uint2 ownTile = texel / T;
    const float4 ownAux = tile_aux.read(ownTile);
    const uint ownCard = uint(round(ownAux.a));

    const float2 g = (float2(texel) + 0.5f) / float(T) - 0.5f;
    const int2 i0 = int2(floor(g));
    const float2 f = fract(g);

    float wsum = 0.0f;
    float3 incident = float3(0.0f);
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int2 ti = clamp(i0 + int2(dx, dy), int2(0), int2(int(tilesX) - 1));
            const float w = (dx == 0 ? (1.0f - f.x) : f.x) * (dy == 0 ? (1.0f - f.y) : f.y);
            if (w <= 0.0f) continue;

            const float4 grid = tile_grid.read(uint2(ti));
            if (grid.a <= 0.0f) continue;                       // never-traced / invalid

            const float4 aux = tile_aux.read(uint2(ti));
            const uint card = uint(round(aux.a));
            if (card != ownCard) continue;                      // Card identity

            const float3 tapN = octDecode(aux.rg);
            if (dot(tapN, ownN) < 0.7f) continue;               // normal compatibility

            const float tapDepth = aux.b;
            const float ownDepth = depth_atlas.read(texel).r;
            if (fabs(tapDepth - ownDepth) > 2.0f * max(params.hit_band, 0.30f)) continue;

            incident += grid.rgb * w;
            wsum += w;
        }
    }

    float3 outRadiance = float3(0.0f);
    float outValid = 0.0f;
    if (wsum > 0.0f) {
        outRadiance = (incident / wsum) * albedo;   // material multiply lives here
        outValid = 1.0f;
    }
    indirect_out.write(float4(outRadiance, outValid), texel);
}
