#ifndef DDGI_MEASURED_SURFACE_METAL
#define DDGI_MEASURED_SURFACE_METAL
#include "SurfaceCacheData.metal"

// Frozen geometry-primary Finalize qualification. Matching an owner alone
// never authorizes lighting from a different surface within that instance.
static bool ddgiMeasuredOwnerCandidate(SurfaceCacheCard card, uint owner,
    float3 hitPos, float3 geometricN, float3 rayDir,
    texture2d<float, access::sample> depthAtlas,
    texture2d<float, access::sample> lightingAtlas,
    thread float3& radiance, thread float& key) {
    radiance=float3(0.0f); key=1e30f;
    if (card.mesh_instance_id!=owner || !all(isfinite(hitPos)) || !all(isfinite(geometricN)) ||
        dot(geometricN,geometricN)<0.25f || dot(geometricN,rayDir)>=0.0f || card.resolution==0u ||
        (card.axis_direction&255u)>2u || ((card.axis_direction>>8u)&255u)>1u) return false;
    float3 n=normalize(geometricN),face=scCardFaceNormal(card);
    if (dot(face,n)>-0.5f || dot(face,-rayDir)>=0.0f) return false;
    float2 uv;
    if (!worldToCardUV(hitPos,card,uv) || !all(isfinite(uv))) return false;
    uint2 size=uint2(depthAtlas.get_width(),depthAtlas.get_height());
    if (any(size==uint2(0)) || any(size!=uint2(lightingAtlas.get_width(),lightingAtlas.get_height())) ||
        card.atlas_offset_x>=size.x || card.atlas_offset_y>=size.y ||
        card.resolution>size.x-card.atlas_offset_x || card.resolution>size.y-card.atlas_offset_y) return false;
    uint2 texel=uint2(clamp(uv,float2(card.atlas_offset_x,card.atlas_offset_y),
        float2(card.atlas_offset_x+card.resolution-1u,card.atlas_offset_y+card.resolution-1u)));
    float depth=depthAtlas.read(texel).r; float4 light=lightingAtlas.read(texel);
    if (!isfinite(depth) || depth<0.0f || !all(isfinite(light)) || light.a<=0.5f) return false;
    float3 captured=cardTexelToWorld(texel,depth,card),delta=captured-hitPos;
    float along=dot(delta,n),footprint=2.0f*length(card.extent.xyz)/float(card.resolution);
    if (!all(isfinite(captured)) || abs(along)>0.002f ||
        length(delta-n*along)>max(footprint,0.002f)) return false;
    radiance=light.rgb; key=length(delta-n*along)+abs(along);
    return true; // Valid black is an observation.
}
#endif
