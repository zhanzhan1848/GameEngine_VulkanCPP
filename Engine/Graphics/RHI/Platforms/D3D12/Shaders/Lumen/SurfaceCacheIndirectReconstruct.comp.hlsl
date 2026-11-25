// Vulkan reference: Lumen/SurfaceCacheIndirectReconstruct.comp (surface_cache_indirect_reconstruct).
struct SurfaceCacheParams
{
    uint atlas_size;
    uint page_size;
    uint capture_budget_pages;
    uint max_cards;
    float update_distance;
    float importance_weight;
    uint max_lights_per_tile;
    uint indirect_rays_per_probe;
    float indirect_temporal_weight;
    float indirect_near_distance;
    uint lookup_count;
    uint _pad;
};

struct IndirectResolveParams
{
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
};

cbuffer IndirectResolveParamsBlock : register(b0, space0)
{
    IndirectResolveParams irp_params : packoffset(c0);
};

RWTexture2D<unorm float4> albedo_atlas : register(u1, space0);
Texture2D<float4> normal_atlas : register(t3, space0);
Texture2D<float4> tile_aux : register(t5, space0);
Texture2D<float4> tile_grid : register(t4, space0);
Texture2D<float4> depth_atlas : register(t2, space0);
RWTexture2D<float4> indirect_out : register(u6, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 octDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    // Inverse of the replace-fold in octEncode: n.xy >= 0 subtracts t, else adds.
    n.xy += lerp((float2)(t), (float2)(-t), step((float2)(0.0), n.xy));
    return normalize(n);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 texel = input.dispatchThreadID.xy;
    uint A = irp_params.sc_params.atlas_size;
    if (((texel.x >= A) || (texel.y >= A)))
    {
        return;
    }
    uint T = irp_params.tile_size;
    uint tilesX = A / T;
    float3 albedo = albedo_atlas[int2(texel)].xyz;
    float2 ownEnc = normal_atlas.Load(int3(int2(texel), 0)).xy;
    float2 param = ownEnc;
    float3 _134 = octDecode(param);
    float3 ownN = _134;
    uint2 ownTile = texel / T.xx;
    float4 ownAux = tile_aux.Load(int3(int2(ownTile), 0));
    uint ownCard = uint(round(ownAux.w));
    float2 g = ((float2(texel) + 0.5f.xx) / float(T).xx) - 0.5f.xx;
    int2 i0 = int2(floor(g));
    float2 f = frac(g);
    float wsum = 0.0f;
    float3 incident = 0.0f.xxx;
    float _209;
    float _221;
    for (int dy = 0; dy < 2; dy++)
    {
        for (int dx = 0; dx < 2; dx++)
        {
            int2 ti = clamp(i0 + int2(dx, dy), int2(0, 0), int(tilesX - 1u).xx);
            if (dx == 0)
            {
                _209 = 1.0f - f.x;
            }
            else
            {
                _209 = f.x;
            }
            if (dy == 0)
            {
                _221 = 1.0f - f.y;
            }
            else
            {
                _221 = f.y;
            }
            float w = _209 * _221;
            if (w <= 0.0f)
            {
                continue;
            }
            float4 grid = tile_grid.Load(int3(ti, 0));
            if (grid.w <= 0.0f)
            {
                continue;
            }
            float4 aux = tile_aux.Load(int3(ti, 0));
            uint card = uint(round(aux.w));
            if (card != ownCard)
            {
                continue;
            }
            float2 param_1 = aux.xy;
            float3 _267 = octDecode(param_1);
            float3 tapN = _267;
            if (dot(tapN, ownN) < 0.7f)
            {
                continue;
            }
            float tapDepth = aux.z;
            float ownDepth = depth_atlas.Load(int3(int2(texel), 0)).x;
            float dDepth = abs(tapDepth - ownDepth);
            if (dDepth > (2.0f * max(irp_params.hit_band, 0.3f)))
            {
                continue;
            }
            incident += (grid.xyz * w);
            wsum += w;
        }
    }
    float3 outRadiance = 0.0f.xxx;
    float outValid = 0.0f;
    if (wsum > 0.0f)
    {
        outRadiance = (incident / wsum.xxx) * albedo;
        outValid = 1.0f;
    }
    indirect_out[int2(texel)] = float4(outRadiance, outValid);
}
