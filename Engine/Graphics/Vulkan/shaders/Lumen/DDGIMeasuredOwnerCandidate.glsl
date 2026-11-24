// Geometry-primary radiance has a stricter source contract than a coarse SDF
// hit. The owner tag is necessary, never sufficient, surface qualification.
bool ddgiMeasuredOwnerCandidate(SurfaceCacheCard card, uint owner, bool ownerKnown,
                                vec3 hitPos, vec3 geometricN, vec3 rayDir,
                                out vec3 radiance, out float key, out uint reject) {
    radiance=vec3(0.0);key=1e30;reject=11u;
    if(!ownerKnown || card.mesh_instance_id!=owner) return false;
    if(any(isnan(hitPos))||any(isinf(hitPos))||any(isnan(geometricN))||any(isinf(geometricN))||
       dot(geometricN,geometricN)<0.25 || dot(geometricN,rayDir)>=0.0 || card.resolution==0u ||
       (card.axis_direction & 255u)>2u || ((card.axis_direction>>8u)&255u)>1u) return false;
    vec3 n=normalize(geometricN), face=scCardFaceNormal(card);
    // A soft key penalty is not same-side geometric normal qualification.
    if(dot(face,n)>-0.5 || dot(face,-rayDir)>=0.0) {reject=12u;return false;}
    vec2 uv;
    if(!worldToCardUV(hitPos,card,uv)||any(isnan(uv))||any(isinf(uv))) {reject=13u;return false;}
    reject=14u; // atlas/depth/lighting stage reached; predicates unchanged
    ivec2 ds=textureSize(depth_atlas,0),ls=textureSize(lighting_atlas,0);
    if(any(lessThanEqual(ds,ivec2(0)))||any(notEqual(ds,ls))||
       card.atlas_offset_x>=uint(ds.x)||card.atlas_offset_y>=uint(ds.y)||
       card.resolution>uint(ds.x)-card.atlas_offset_x||
       card.resolution>uint(ds.y)-card.atlas_offset_y) return false;
    uvec2 texel=uvec2(clamp(uv,vec2(card.atlas_offset_x,card.atlas_offset_y),
        vec2(card.atlas_offset_x+card.resolution-1u,card.atlas_offset_y+card.resolution-1u)));
    float depth=texelFetch(depth_atlas,ivec2(texel),0).r;
    vec4 light=texelFetch(lighting_atlas,ivec2(texel),0);
    if(isnan(depth)||isinf(depth)||depth<0.0||any(isnan(light))||any(isinf(light))||light.a<=0.5) {
        reject=14u;return false;
    }
    vec3 captured=cardTexelToWorld(texel,depth,card),delta=captured-hitPos;
    float along=dot(delta,n);
    float footprint=2.0*length(card.extent.xyz)/float(card.resolution);
    // 2mm is a geometry/raster plane gate; no .45m SDF uncertainty is added.
    if(any(isnan(captured))||any(isinf(captured))||abs(along)>0.002||
       length(delta-n*along)>max(footprint,0.002)) {reject=15u;return false;}
    radiance=light.rgb;key=length(delta-n*along)+abs(along);reject=0u;
    return true; // includes valid black; a failed source never changes depth
}
