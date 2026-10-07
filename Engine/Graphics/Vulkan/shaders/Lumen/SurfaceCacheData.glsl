#ifndef SURFACE_CACHE_DATA_GLSL
#define SURFACE_CACHE_DATA_GLSL

// The C-series query contract (scQueryCardCandidate) reads atlases through
// samplerless texelFetch; enable the extension here so every consumer gets it
// (duplicate enable in the including shader is legal).
#extension GL_EXT_samplerless_texture_functions : enable

// Shared structs + helpers for SurfaceCache Vulkan GLSL shaders.
// Ported from Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal.
// All vec3 → vec4 for 16-byte alignment (matches Metal float4 / C++ math::v4 pattern).

// Matches C++ SurfaceCacheCard (64 bytes).
struct SurfaceCacheCard {
    vec4  center;          // xyz = center, w unused
    vec4  extent;          // xyz = half-extents, w unused
    uint  axis_direction;  // low byte = axis, high byte = direction
    uint  resolution;
    uint  atlas_offset_x;
    uint  atlas_offset_y;
    uint  mesh_instance_id;
    // [SC per-mesh 计划 P2] 静态页身份（与 C++ SurfaceCacheTypes.h 同位）：
    uint  static_mesh_slot;   // SurfaceStaticPages 槽位；0xFFFFFFFF = 非静态
    uint  flags;              // bit0 = static（表面来自离线 owner 页）
    uint  page_card;          // 页在 mesh bake.cards 内的序号
};

// Matches C++ SurfaceCacheCardLookup (48 bytes).
struct SurfaceCacheCardLookup {
    vec4  aabb_min;    // xyz = min, w unused
    vec4  aabb_max;    // xyz = max, w unused
    uint  card_start;
    uint  card_count;
    uint  _pad0;
    uint  _pad1;
};

// Matches C++ SurfaceCacheParams (48 bytes).
struct SurfaceCacheParams {
    uint  atlas_size;
    uint  page_size;
    uint  capture_budget_pages;
    uint  max_cards;
    float update_distance;
    float importance_weight;
    uint  max_lights_per_tile;
    uint  indirect_rays_per_probe;
    float indirect_temporal_weight;
    float indirect_near_distance;
    uint  lookup_count;
    uint  _pad;
};

// Matches Metal LightInfo (48 bytes).
struct LightInfo {
    vec4  position;    // xyz = position, w = radius
    vec4  color;       // xyz = color, w = unused
    vec4  direction;   // xyz = direction, w = type (as float)
};

// Matches C++ CardDispatchInfo (32 bytes).
struct CardDispatchInfo {
    uint texel_offset;
    uint texel_count;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint card_index;
    uint _pad1;
    uint _pad2;
};

// Matches C++ FlattenedLightingParams (96 bytes; the trailing 32B probe-feed
// block is optional — shaders that ignore it still read the first 64B fine).
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;
    uint total_texels;
    uint card_count;
    uint light_count;
    // 能量所有权 Phase 4a: openSky V2 开关（E_geo/E_sky 拆分消费；
    // PRIMAL_GI_OPENSKY_V2=0 → 0 = 旧整探针分类）。
    uint opensky_v2;
    vec4 ddgi_origin_spacing;   // xyz origin, w spacing
    uvec4 ddgi_counts_valid;    // xyz counts, w valid flag
    // T3.3: SC Radiosity gate (x = on) — see Metal twin.
    uvec4 radiosity_gate;       // x = indirect_radiosity_on, y = radiosity weight 档,
                                // z = tile size, w = openSky 分类开关（GI 根治 Phase 3.2）
    // WRC 计划 Stream B1/B2（能量单向化）— 与 C++ SurfaceCacheTypes.h 同步。
    float probe_feed_bootstrap; // 引导因子（PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1：窗口内 1→0）
    uint  radiosity_solo;       // 1 = tile radiosity 单轨（volume 外 texel 间接唯一来源）
    // [WRC 计划 Stream D2]（PRIMAL_SC_PAGE_TABLE=1）: 过表寻址开关。
    uint  page_table_enabled;
};

// ============================================================================
// [WRC 计划 Stream D2]: 页表过表寻址——页表模式（pageTableEnabled）下卡片
// 的 atlas 原点经 card_page_table 条目解析（x_page<<16|y_page，乘 page_size
// 得 texel 原点），替换直读 card.atlas_offset_{x,y}；下游 worldToCardUV /
// cardTexelToWorld / scQueryCardCandidate 全部消费解析后的卡片——采样与
// resolve 查询契约由此统一过表。关闭时原样返回（逐位旧行为）。
// ============================================================================
SurfaceCacheCard scResolveCardOrigin(SurfaceCacheCard card, bool pageTableEnabled,
                                     uint pageEntry, uint page_size) {
    if (pageTableEnabled) {
        card.atlas_offset_x = ((pageEntry >> 16u) & 0xFFFFu) * page_size;
        card.atlas_offset_y = (pageEntry & 0xFFFFu) * page_size;
    }
    return card;
}

// Reconstruct world position from card atlas UV + depth.
vec3 cardTexelToWorld(uvec2 atlas_uv, float depth, SurfaceCacheCard card) {    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    vec3 local_pos;
    if (axis == 0u) {
        local_pos.x = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (axis == 1u) {
        local_pos.y = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else {
        local_pos.z = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center.xyz;
}

// [P0 2026-09-20] 带 UV 余量的投影：margin_uv 为归一化余量（0 = 严格边界）。
// [R1 2026-09-20] worldToCardUVMarginPad：margin_uv（texel 贴边）+
// pad_m（米制接受域扩张）双参数。pad 管整段掠射带（SDF 命中带 ±0.45m 在
// 卡片边缘外侧的投影），margin 管贴边 texel；按查询轴逐轴换算
// pad_m/(2*extent)，extent→0 时该轴 pad 退化为 0（退化薄卡不横扩）。
// 捕获侧 extent 不动，不摊薄 texel 密度。钳到边缘 texel 采样：正确性仍由
// 朝向门 + along/perp 误差检查把关（远处几何投影会被 perp 拒绝），只是
// 不再拒绝"贴边一个 texel / 掠射带内"的合法候选。捕获侧 projectToCard
// 保持严格边界不受影响。
// （GLSL 先声明后使用：Pad 版本在前，worldToCardUV / worldToCardUVMargin 委托之。）
bool worldToCardUVMarginPad(vec3 world_pos, SurfaceCacheCard card, out vec2 out_uv,
                            float margin_uv, float pad_m) {
    vec3 local = world_pos - card.center.xyz;
    float u, v, depth;

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    if (axis == 0u) {
        depth = local.x * (dir > 0u ? 1.0 : -1.0);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (axis == 1u) {
        depth = local.y * (dir > 0u ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * (dir > 0u ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0) return false;
    // R1: 米制 pad 逐轴换算为归一化域。
    float padU = 0.0, padV = 0.0;
    if (pad_m > 0.0) {
        float eu = (axis == 0u) ? card.extent.y : (axis == 1u ? card.extent.x : card.extent.x);
        float ev = (axis == 0u) ? card.extent.z : (axis == 1u ? card.extent.z : card.extent.y);
        padU = (eu > 1e-4) ? pad_m / (eu * 2.0) : 0.0;
        padV = (ev > 1e-4) ? pad_m / (ev * 2.0) : 0.0;
    }
    if (u < -margin_uv - padU || u >= 1.0 + margin_uv + padU ||
        v < -margin_uv - padV || v >= 1.0 + margin_uv + padV) return false;
    // C6: u/v==1.0 bleeds into the next card — clamp into [0, 1-ε].
    u = clamp(u, 0.0, 0.999999);
    v = clamp(v, 0.0, 0.999999);

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

bool worldToCardUVMargin(vec3 world_pos, SurfaceCacheCard card, out vec2 out_uv,
                         float margin_uv) {
    return worldToCardUVMarginPad(world_pos, card, out_uv, margin_uv, 0.0);
}

// Find atlas UV for a world position given a card. Returns true if inside.
bool worldToCardUV(vec3 world_pos, SurfaceCacheCard card, out vec2 out_uv) {
    return worldToCardUVMargin(world_pos, card, out_uv, 0.0);
}

// ============================================================================
// C (plan §三.C): unified Surface Cache query contract.
//
// All SC consumers (SurfaceCacheIndirectResolve, DDGITraceFinalize,
// ScreenProbeTraceFinalize) validate candidates the same way:
//   1. bounds        — worldToCardUVMargin (UV coverage on the card plane,
//                      1-texel edge margin for the SDF hit-band smear)
//   2. orientation   — the captured surface must face the QUERY side:
//                      dot(faceNormal, surfaceFacing) > 0 (surfaceFacing is
//                      -rayDir for ray queries, the receiver normal for tile
//                      queries). Two-sided walls get distinct cards per side;
//                      a bounds-passing card facing away describes the wrong
//                      face.
//   3. capture valid — depth texel finite and ≥ 0
//   4. decomposed error vs the reconstructed captured position
//                      (cardTexelToWorld): |along| (card axis depth mismatch —
//                      covers the caller's SDF hit band) and perp (lateral —
//                      must stay inside the texel footprint). The old
//                      extent-proportional single-sided tolerance accepted
//                      captures metres in front of interior hits on big AABB
//                      cards.
//   5. stable choice — min key = perp + 0.5·|along| across ALL candidates
//                      (never first-hit / card-list order dependent)
//   6. valid black   — accepted (a black texel is a measurement); only
//                      NaN/Inf candidate radiance is rejected.
// ============================================================================
vec3 scCardFaceNormal(SurfaceCacheCard card) {
    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8u) & 0xFFu;
    float s = (dir > 0u) ? 1.0 : -1.0;
    return (axis == 0u) ? vec3(s, 0.0, 0.0)
         : (axis == 1u) ? vec3(0.0, s, 0.0)
                        : vec3(0.0, 0.0, s);
}

// Every texel within the world-distance tolerance must lie inside this
// projected rectangle. Include a one-texel rounding guard, then clamp to the
// owner's allocation; the caller still checks identity and full 3D distance.
bool scGeometryCacheTexelBounds(vec2 atlasUV, SurfaceCacheCard card, float tolerance,
                               out ivec2 minimum, out ivec2 maximum) {
    uint axis=card.axis_direction&255u;
    if(axis>2u || card.resolution==0u || any(isnan(atlasUV)) || any(isinf(atlasUV))) return false;
    vec2 extent=vec2(card.extent[axis==0u?1u:0u],card.extent[axis==2u?1u:2u]);
    if(any(isnan(extent)) || any(isinf(extent)) || any(lessThanEqual(extent,vec2(0)))) return false;
    vec2 radius=min(vec2(float(card.resolution)),vec2(tolerance)*float(card.resolution)/(2.*extent));
    ivec2 ownerMin=ivec2(card.atlas_offset_x,card.atlas_offset_y);
    ivec2 ownerMax=ownerMin+ivec2(card.resolution)-ivec2(1);
    minimum=max(ownerMin,ivec2(ceil(atlasUV-vec2(.5)-radius))-ivec2(1));
    maximum=min(ownerMax,ivec2(floor(atlasUV-vec2(.5)+radius))+ivec2(1));
    return all(lessThanEqual(minimum,maximum));
}

// Interpolation is restricted to four samples of the SAME original triangle.
// Their convex combination stays on that triangle (including sloped planes),
// unlike filtering across identity boundaries or merely enlarging tolerance.
bool scGeometryBilinearFootprint(vec2 atlasUV, SurfaceCacheCard card,
                                out ivec2 base, out vec4 weights) {
    base=ivec2(0); weights=vec4(0);
    if(card.resolution<2u || any(isnan(atlasUV)) || any(isinf(atlasUV))) return false;
    vec2 samplePosition=atlasUV-.5;
    ivec2 ownerMin=ivec2(card.atlas_offset_x,card.atlas_offset_y);
    ivec2 ownerMax=ownerMin+ivec2(card.resolution)-ivec2(1);
    if(any(lessThan(samplePosition,vec2(ownerMin))) ||
       any(greaterThanEqual(samplePosition,vec2(ownerMax)))) return false;
    base=ivec2(floor(samplePosition));vec2 f=samplePosition-vec2(base);
    weights=vec4((1.-f.x)*(1.-f.y),f.x*(1.-f.y),(1.-f.x)*f.y,f.x*f.y);
    return true;
}
bool scGeometryBilinearSurface(vec2 atlasUV,SurfaceCacheCard card,uint triangle,
                              uvec4 identities,vec4 depths,vec3 hitPos,
                              out vec3 captured,out vec4 weights) {
    captured=vec3(0);weights=vec4(0);ivec2 base;
    if(triangle==0xffffffffu || any(notEqual(identities,uvec4(triangle))) ||
       any(isnan(depths)) || any(isinf(depths)) || any(lessThan(depths,vec4(0))) ||
       any(isnan(hitPos)) || any(isinf(hitPos)) ||
       !scGeometryBilinearFootprint(atlasUV,card,base,weights)) return false;
    captured=weights.x*cardTexelToWorld(uvec2(base),depths.x,card)
            +weights.y*cardTexelToWorld(uvec2(base+ivec2(1,0)),depths.y,card)
            +weights.z*cardTexelToWorld(uvec2(base+ivec2(0,1)),depths.z,card)
            +weights.w*cardTexelToWorld(uvec2(base+ivec2(1,1)),depths.w,card);
    float error=length(captured-hitPos);
    return !isnan(error) && !isinf(error) && error<=.05;
}

// Three matching corners also span a valid convex subset of the original
// triangle. Negative weights reject extrapolation across the missing corner.
bool scGeometrySimplexSurface(vec2 atlasUV,SurfaceCacheCard card,uint triangle,
                             uvec4 identities,vec4 depths,vec3 hitPos,
                             out vec3 captured,out vec4 weights) {
    captured=vec3(0);weights=vec4(0);ivec2 base;vec4 unused;
    if(triangle==0xffffffffu || any(isnan(hitPos)) || any(isinf(hitPos)) ||
       !scGeometryBilinearFootprint(atlasUV,card,base,unused)) return false;
    uint count=0u,missing=0u;
    for(uint k=0u;k<4u;++k) {
        bool valid=identities[k]==triangle && !isnan(depths[k]) && !isinf(depths[k]) && depths[k]>=0.;
        if(valid) ++count; else missing=k;
    }
    if(count!=3u) return false;
    vec2 f=atlasUV-.5-vec2(base);
    if(missing==0u) weights=vec4(0,1.-f.y,1.-f.x,f.x+f.y-1.);
    else if(missing==1u) weights=vec4(1.-f.y,0,f.y-f.x,f.x);
    else if(missing==2u) weights=vec4(1.-f.x,f.x-f.y,0,f.y);
    else weights=vec4(1.-f.x-f.y,f.x,f.y,0);
    if(any(lessThan(weights,vec4(0)))) return false;
    for(uint k=0u;k<4u;++k) if(weights[k]>0.) {
        ivec2 offset=ivec2(int(k&1u),int(k>>1u));
        captured+=weights[k]*cardTexelToWorld(uvec2(base+offset),depths[k],card);
    }
    float error=length(captured-hitPos);
    return !isnan(error) && !isinf(error) && error<=.05;
}

// A same-primitive segment is a convex subset even when the footprint has
// only two valid corners. Clamp to the segment; the full 3D .05m bound still
// rejects positions outside its finite support. No texture-edge extrapolation.
bool scGeometrySegmentSurface(vec2 atlasUV,SurfaceCacheCard card,uint triangle,
                             uvec4 identities,vec4 depths,vec3 hitPos,
                             out vec3 captured,out vec4 weights) {
    captured=vec3(0);weights=vec4(0);ivec2 base;vec4 unused;
    if(triangle==0xffffffffu || any(isnan(hitPos)) || any(isinf(hitPos)) ||
       !scGeometryBilinearFootprint(atlasUV,card,base,unused)) return false;
    float best=.05;bool found=false;
    for(uint a=0u;a<3u;++a) for(uint b=a+1u;b<4u;++b) {
        if(identities[a]!=triangle || identities[b]!=triangle ||
           isnan(depths[a]) || isinf(depths[a]) || depths[a]<0. ||
           isnan(depths[b]) || isinf(depths[b]) || depths[b]<0.) continue;
        vec3 first=cardTexelToWorld(uvec2(base+ivec2(int(a&1u),int(a>>1u))),depths[a],card);
        vec3 last=cardTexelToWorld(uvec2(base+ivec2(int(b&1u),int(b>>1u))),depths[b],card);
        vec3 edge=last-first;float squared=dot(edge,edge);
        if(isnan(squared) || isinf(squared) || squared<=1e-20) continue;
        float t=dot(hitPos-first,edge)/squared;
        if(isnan(t) || isinf(t)) continue;
        t=clamp(t,0.,1.);vec3 point=first+t*edge;
        float error=length(point-hitPos);
        if(isnan(error) || isinf(error) || error>best || (found && error==best)) continue;
        best=error;found=true;captured=point;weights=vec4(0);weights[a]=1.-t;weights[b]=t;
    }
    return found;
}

// Wider finite support still stays within one owner allocation. The query
// retains the original interior 2x2 footprint; only its source support expands.
bool scGeometryExpandedBounds(vec2 uv,SurfaceCacheCard card,out ivec2 lo,out ivec2 hi) {
    ivec2 base;vec4 weights;lo=hi=ivec2(0);
    if(!scGeometryBilinearFootprint(uv,card,base,weights)) return false;
    ivec2 ownerMin=ivec2(card.atlas_offset_x,card.atlas_offset_y);
    lo=max(base-ivec2(1),ownerMin);
    hi=min(base+ivec2(2),ownerMin+ivec2(card.resolution)-ivec2(1));
    return true;
}
bool scGeometrySegmentPair(SurfaceCacheCard card,ivec2 a,ivec2 b,uint triangle,
                           uvec2 identities,vec2 depths,vec3 hitPos,
                           out vec3 captured,out float weight) {
    captured=vec3(0);weight=0.;
    ivec2 lo=ivec2(card.atlas_offset_x,card.atlas_offset_y),hi=lo+ivec2(card.resolution)-ivec2(1);
    if(card.resolution<2u || triangle==0xffffffffu || any(notEqual(identities,uvec2(triangle))) ||
       any(lessThan(a,lo)) || any(greaterThan(a,hi)) || any(lessThan(b,lo)) || any(greaterThan(b,hi)) ||
       any(isnan(depths)) || any(isinf(depths)) || any(lessThan(depths,vec2(0))) ||
       any(isnan(hitPos)) || any(isinf(hitPos))) return false;
    vec3 first=cardTexelToWorld(uvec2(a),depths.x,card),last=cardTexelToWorld(uvec2(b),depths.y,card);
    vec3 edge=last-first;float squared=dot(edge,edge);
    if(isnan(squared) || isinf(squared) || squared<=1e-20) return false;
    float t=dot(hitPos-first,edge)/squared;
    if(isnan(t) || isinf(t)) return false;
    weight=clamp(t,0.,1.);captured=first+weight*edge;
    float error=length(captured-hitPos);
    return !isnan(error) && !isinf(error) && error<=.05;
}

// Sorted pixel coordinates form a bounded convex hull. Integer orientation
// avoids cancellation and removes collinear interior points. count <= 64.
int scSupportCross(ivec2 a,ivec2 b,ivec2 c) {
    ivec2 ab=b-a,ac=c-a;return ab.x*ac.y-ab.y*ac.x;
}
uint scSupportHull(ivec2 coords[64],uint count,out uint hull[128]) {
    if(count==0u) return 0u;
    uint k=0u;
    for(uint i=0u;i<count;++i) {
        while(k>=2u && scSupportCross(coords[hull[k-2u]],coords[hull[k-1u]],coords[i])<=0) --k;
        hull[k++]=i;
    }
    uint lower=k+1u;
    for(int i=int(count)-2;i>=0;--i) {
        while(k>=lower && scSupportCross(coords[hull[k-2u]],coords[hull[k-1u]],coords[i])<=0) --k;
        hull[k++]=uint(i);
    }
    return k>1u?k-1u:k;
}
bool scGeometryHullBounds(vec2 uv,SurfaceCacheCard card,out ivec2 lo,out ivec2 hi) {
    ivec2 base;vec4 weights;lo=hi=ivec2(0);
    if(!scGeometryBilinearFootprint(uv,card,base,weights)) return false;
    ivec2 ownerMin=ivec2(card.atlas_offset_x,card.atlas_offset_y);
    lo=max(base-ivec2(3),ownerMin);
    hi=min(base+ivec2(4),ownerMin+ivec2(card.resolution)-ivec2(1));
    return true;
}
bool scGeometryTriangleSupport(vec2 uv,SurfaceCacheCard card,ivec2 a,ivec2 b,ivec2 c,
                               uint triangle,uvec3 identities,vec3 depths,vec3 hitPos,
                               out vec3 captured,out vec3 weights) {
    captured=vec3(0);weights=vec3(0);ivec2 base;vec4 unused;
    ivec2 lo=ivec2(card.atlas_offset_x,card.atlas_offset_y),hi=lo+ivec2(card.resolution)-ivec2(1);
    if(triangle==0xffffffffu || any(notEqual(identities,uvec3(triangle))) ||
       any(lessThan(a,lo)) || any(greaterThan(a,hi)) || any(lessThan(b,lo)) || any(greaterThan(b,hi)) ||
       any(lessThan(c,lo)) || any(greaterThan(c,hi)) ||
       any(isnan(depths)) || any(isinf(depths)) || any(lessThan(depths,vec3(0))) ||
       any(isnan(hitPos)) || any(isinf(hitPos)) ||
       !scGeometryBilinearFootprint(uv,card,base,unused)) return false;
    int determinant=scSupportCross(a,b,c);if(determinant==0) return false;
    vec2 ab=vec2(b-a),ac=vec2(c-a),q=uv-.5-vec2(a);
    float wb=(q.x*ac.y-q.y*ac.x)/float(determinant);
    float wc=(ab.x*q.y-ab.y*q.x)/float(determinant);
    weights=vec3(1.-wb-wc,wb,wc);
    if(any(isnan(weights)) || any(isinf(weights)) || any(lessThan(weights,vec3(0)))) return false;
    if(weights.x>0.) captured+=weights.x*cardTexelToWorld(uvec2(a),depths.x,card);
    if(weights.y>0.) captured+=weights.y*cardTexelToWorld(uvec2(b),depths.y,card);
    if(weights.z>0.) captured+=weights.z*cardTexelToWorld(uvec2(c),depths.z,card);
    float error=length(captured-hitPos);
    return !isnan(error) && !isinf(error) && error<=.05;
}

// One-candidate evaluation. Returns false when the candidate is rejected;
// on true, outRadiance carries the texel radiance (may be black) and outKey
// the selection key. tolFloor covers the caller's query-position error band
// (SDF march threshold); footprint covers texel quantization.
// qpadM（R1 2026-09-20）: 查询侧 UV 接受域米制 pad（0 = 旧行为）——见
// worldToCardUVMarginPad 注。
// hitNormalHint（G2，2026-09-16）: 命中点 SDF 梯度（指向空气），长度² > 0.25
// 时启用朝向身份门。粗 SDF march 的 ±voxel×0.5 误差带内，平行近邻面（拱腹
// vs 受光外墙）位置不可分、朝向类可分——拱顶热点借光的治理手段。
// vec3(0) = 不启用（调用方无 SDF 法线来源时保持原行为）。
//
// [R0 2026-09-20] 漏斗细分：scQueryCardCandidateDbg 经 outReject 报告拒绝级
// （0=接受 1=bounds 2=orient-朝向 7=orient-hint(G2) 3=depth 无效 4=along
// 5=perp 6=NaN），带外拒绝原因计数不再要求调用方复制谓词。
// [①orientFail 治理 2026-09-21] 原合并码 2 拆成 2（face×ray 朝向）与
// 7（face×SDF-hint G2 门）——漏斗数据显示 orientFail 47.9% 为 SPGI 第一
// 拒绝源，但无法定位是"背对卡拒绝"（几何上正常：薄片墙两侧卡）还是
// "hint 门过杀"。WRC 侧计数消费方已同步把 7 计入 orient。
// scQueryCardCandidate 保持原签名语义。
bool scQueryCardCandidateDbg(SurfaceCacheCard card,
                             vec3 hitPos,
                             vec3 surfaceFacing,
                             float tolFloor,
                             float qpadM,
                             texture2D depthAtlas,
                             texture2D lightingAtlas,
                             vec3 hitNormalHint,
                             out vec3 outRadiance,
                             out float outKey,
                             out uint outReject) {
    outRadiance = vec3(0.0);
    outKey = 1e30;
    outReject = 0u;

    // [P0 2026-09-20] 1 texel UV margin + [R1] 米制 pad——SDF 命中带的横向
    // smear 与掠射带（漏斗 boundsFail 主源）；正确性由下述朝向/误差检查
    // 继续把关。
    vec2 atlasUV;
    if (!worldToCardUVMarginPad(hitPos, card, atlasUV,
                                1.0 / float(max(card.resolution, 1u)), qpadM)) {
        outReject = 1u; return false;
    }

    // 2. orientation — the captured surface's VISIBLE side faces the card
    // plane (n_true = -faceNormal; the eye sits at the plane looking along
    // faceNormal). A query legitimately sees the surface only when it
    // arrives from that side: dot(-faceNormal, -surfaceFacing) < 0.
    // (The first draft had this comparison inverted — it rejected every
    // front-face lookup and starved the transport.)
    if (dot(scCardFaceNormal(card), surfaceFacing) >= 0.0) { outReject = 2u; return false; }
    // G2' [①orientFail 治理 2026-09-21]：原 G2 硬门（d = faceN·hint > -0.5
    // 直接拒绝，码 2）在 Sponza 上过杀——漏斗实测 found 1.87%，关掉 hint 门
    // found 3× 但拱顶借光回归。改为**软键惩罚**：hint 错位不再一票否决，
    // 而是给 key 加罚——有对齐替代卡时错位卡必输（拱腹 vs 受光外墙的判别
    // 保留），无替代时仍被接受（横幅/薄几何的唯一候选不再被噪声 hint 杀死）。
    // 罚值 = max(d_scaled + 1, 0)：完美对齐（d=-1）零罚。outReject 仍报 7
    // （旧拒绝条件成立时）供漏斗计数，但不再 return false。
    float hintPenalty = 0.0;
    if (dot(hitNormalHint, hitNormalHint) > 0.25) {
        const float d = dot(scCardFaceNormal(card), hitNormalHint);
        if (d > -0.5) outReject = 7u;   // 计数：旧硬门会拒绝的候选
        hintPenalty = max(d + 1.0, 0.0);
    }

    // 3. capture validity.
    uvec2 texel = uvec2(clamp(atlasUV, vec2(0.0), vec2(card.atlas_offset_x + card.resolution - 1u,
                                                       card.atlas_offset_y + card.resolution - 1u)));
    float depth = texelFetch(depthAtlas, ivec2(texel), 0).r;
    if (isnan(depth) || isinf(depth) || depth < 0.0) { outReject = 3u; return false; }

    // 4. decomposed position error.
    vec3 captured = cardTexelToWorld(texel, depth, card);
    vec3 delta = captured - hitPos;
    vec3 n = scCardFaceNormal(card);
    float along = dot(delta, n);
    vec3 perpV = delta - n * along;
    float perp = length(perpV);
    float footprint = length(card.extent.xyz) / float(max(card.resolution, 1u));
    float tolAlong = tolFloor + footprint;
    // The caller's uncertainty is a ball of radius ~tolFloor (the SDF hit
    // band projects LATERALLY onto grazing-oriented card planes), plus texel
    // quantization laterally. tolPerp floor of tolFloor — not a constant —
    // was measured starving the transport via grazing rejections.
    float tolPerp = max(footprint * 1.5, tolFloor);
    if (abs(along) > tolAlong) { outReject = 4u; return false; }
    if (perp > tolPerp)        { outReject = 5u; return false; }

    // 6. radiance fetch — valid black accepted.
    vec3 c = texelFetch(lightingAtlas, ivec2(texel), 0).rgb;
    if (any(isnan(c)) || any(isinf(c))) { outReject = 6u; return false; }
    outRadiance = c;
    // [①治理] 软键：错位卡在 min-key 竞争中按罚值后置——罚值以位置误差项
    // 的量级为标尺（对齐卡 vs 错位卡同位错时对齐者必胜；罚值不封顶保证
    // 强错位几乎必输于任何对齐候选）。
    outKey = perp + 0.5 * abs(along) + hintPenalty * (perp + abs(along) + max(tolFloor, 0.05));
    return true;
}

bool scQueryCardCandidate(SurfaceCacheCard card,
                          vec3 hitPos,
                          vec3 surfaceFacing,
                          float tolFloor,
                          texture2D depthAtlas,
                          texture2D lightingAtlas,
                          vec3 hitNormalHint,
                          out vec3 outRadiance,
                          out float outKey) {
    uint rejectDummy;
    return scQueryCardCandidateDbg(card, hitPos, surfaceFacing, tolFloor, 0.0,
                                   depthAtlas, lightingAtlas, hitNormalHint,
                                   outRadiance, outKey, rejectDummy);
}

// T1.10: octEncode/octDecode are canonical in ShaderLib/MathLib.glsl now.
// Textual include keeps this header self-contained for existing consumers.
#include "ShaderLib/MathLib.glsl"

// Hash function for reproducible random (ported from CommonFunction.metal::hash).
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// The float/sin hash above loses useful low bits for tile indices near the
// end of a 2048² atlas: all 32 rays of tile (245,240) collapsed to one
// tangent direction. Integer avalanche keeps each ray distinct and the
// trace/resolve reconstruction bit-identical. Top 24 bits convert exactly
// to a float in [0,1), excluding the endpoint 1.
uint scTileHash32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

vec2 scTileRayXi(uint tileIndex, uint rayIndex, uint frameIndex) {
    uint seed = tileIndex * 0x9e3779b9u ^ rayIndex * 0x85ebca6bu ^
                frameIndex * 0xc2b2ae35u;
    return vec2(float(scTileHash32(seed ^ 0x68bc21ebu) >> 8u),
                float(scTileHash32(seed ^ 0x02e5be93u) >> 8u)) *
           (1.0 / 16777216.0);
}

// ============================================================================
// Uniform card grid (T1.3) — canonical query helpers.
//
// The grid_offsets SSBO starts with a 16-u32 header (SurfaceCacheCardGridHeader
// on the C++ side), followed by (cell_count + 1) prefix-sum u32s into the
// grid_cards SSBO. Consumers declare their own buffer blocks (binding numbers
// differ per pass) and use these helpers for decode + cell lookup.
// ============================================================================

struct SCGridParams {
    vec3  grid_min;
    vec3  grid_max;
    uvec3 dims;
    float cell_size;
    uint  card_count;
};

// Decode the 3 header uvec4s (words 0..11 of grid_offsets).
SCGridParams scGridDecode(uvec4 h0, uvec4 h1, uvec4 h2) {
    SCGridParams gp;
    gp.grid_min = uintBitsToFloat(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = uintBitsToFloat(h1.xyz);
    gp.cell_size = uintBitsToFloat(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

// World position → linear cell index; 0xFFFFFFFF when outside the grid.
uint scGridCellIndex(SCGridParams gp, vec3 p) {
    vec3 g = (p - gp.grid_min) / gp.cell_size;
    if (g.x < 0.0 || g.y < 0.0 || g.z < 0.0) return 0xFFFFFFFFu;
    uvec3 c = uvec3(g);
    if (c.x >= gp.dims.x || c.y >= gp.dims.y || c.z >= gp.dims.z) return 0xFFFFFFFFu;
    return c.x + gp.dims.x * (c.y + gp.dims.y * c.z);
}

// ============================================================================
// [SC per-mesh 计划 P4] 射线最近有效表面（PRIMAL_SC_RAY_LOOKUP=1，默认关）。
//
// 计划 §6 流程的单卡部分：射线 × 卡平面投影 → depth/mask 校验 → 重建真实
// 表面点 → t_surface 与横向残差校验。与 scQueryCardCandidate（点查询）的
// 本质差别：这里由射线自身产生 t_surface（最近有效表面），而不是"SDF 命中
// 点附近找一张卡"。bbox 级候选与近远序遍历在消费方（各自的 lookups SSBO
// 绑定号不同，无法在此头里直接引用）。
//
// 背面规则：scCardFaceNormal 返回 capture inward 方向 fwd，
// 接受 dot(D,fwd)>0；它不是几何表面法线。
// mask 语义：depth < 0 / NaN = 无效 texel（AtlasInit 种子 + 静态页显式无效）。
// ============================================================================
bool scRayCardSurfaceDDA(SurfaceCacheCard card, vec3 rayO, vec3 rayD,
                       float t_min, float t_max, float tol_band,
                       texture2D depthAtlas,
                       out float out_t, out vec3 out_pos, out vec2 out_atlas_uv,
                       out uint out_reject)
{
    out_t = 0.0; out_pos = vec3(0.0); out_atlas_uv = vec2(0.0); out_reject = 0u;
    // scCardFaceNormal is the inward CAPTURE direction, not the surface normal.
    if (any(isnan(rayO)) || any(isinf(rayO)) || any(isnan(rayD)) || any(isinf(rayD))) { out_reject=6u; return false; }
    vec3 fwd = scCardFaceNormal(card);
    float dn = dot(rayD, fwd);
    if (abs(dn) < 1e-5) { out_reject = 1u; return false; }
    if (dn <= 0.0) { out_reject = 2u; return false; }
    uint axis = card.axis_direction & 0xFFu;
    uint ua = axis == 0u ? 1u : 0u;
    uint va = axis == 2u ? 1u : 2u;
    vec2 extent = vec2(card.extent[ua], card.extent[va]);
    if (card.resolution == 0u || any(lessThanEqual(extent,vec2(0))) ||
        any(isnan(card.center)) || any(isinf(card.center)) || any(isnan(card.extent)) || any(isinf(card.extent))) {
        out_reject=6u; return false;
    }
    float plane = dot(card.center.xyz - rayO, fwd);
    vec3 relative=rayO-card.center.xyz;
    vec2 uvO=vec2(relative[ua],relative[va])/(2.*extent)+.5;
    vec2 uvD=vec2(rayD[ua],rayD[va])/(2.*extent);
    float enter=max(t_min,plane/dn);
    float leave=min(t_max,(plane+2.*card.extent[axis]+.5)/dn);
    for(uint a=0u;a<2u;++a) {
        if(abs(uvD[a])<1e-12) {
            if(uvO[a]<0. || uvO[a]>1.) {out_reject=4u;return false;}
        } else {
            float a0=(0.-uvO[a])/uvD[a], a1=(1.-uvO[a])/uvD[a];
            enter=max(enter,min(a0,a1)); leave=min(leave,max(a0,a1));
        }
    }
    if(enter>leave) {out_reject=4u;return false;}
    int res=int(card.resolution);
    ivec2 cell=ivec2(clamp(floor((uvO+uvD*enter)*float(res)),vec2(0),vec2(res-1)));
    ivec2 step=ivec2(sign(uvD));
    vec2 nextBoundary=vec2(1e30), increment=vec2(1e30);
    for(uint a=0u;a<2u;++a) if(step[a]!=0) {
        float boundary=float(cell[a]+(step[a]>0?1:0))/float(res);
        nextBoundary[a]=(boundary-uvO[a])/uvD[a];
        increment[a]=1./(float(res)*abs(uvD[a]));
    }
    float footprint=length(card.extent.xyz)/float(card.resolution);
    bool validSeen=false;
    // Monotone UV traversal crosses at most 2*resolution cells. No root
    // iteration, initial-hole rejection, or depth-discontinuity oscillation.
    for(uint iteration=0u;iteration<2u*card.resolution+2u;++iteration) {
        if(any(lessThan(cell,ivec2(0))) || any(greaterThanEqual(cell,ivec2(res)))) break;
        float exitT=min(leave,min(nextBoundary.x,nextBoundary.y));
        uvec2 texel=uvec2(cell)+uvec2(card.atlas_offset_x,card.atlas_offset_y);
        float depth=texelFetch(depthAtlas,ivec2(texel),0).r;
        if(!isnan(depth) && !isinf(depth) && depth>=0.) {
            validSeen=true;
            float t=(plane+depth)/dn;
            if(t>=enter-1e-5 && t<=exitT+1e-5 && t>=t_min && t<=t_max) {
                vec3 p=cardTexelToWorld(texel,depth,card);
                if(length(p-(rayO+rayD*t))<=max(footprint*1.5,1e-4)) {
                    out_t=t;out_pos=p;out_atlas_uv=vec2(texel)+.5;return true;
                }
            }
        }
        if(exitT>=leave) break;
        bool crossX=nextBoundary.x<=exitT+1e-6;
        bool crossY=nextBoundary.y<=exitT+1e-6;
        if(crossX){cell.x+=step.x;nextBoundary.x+=increment.x;}
        if(crossY){cell.y+=step.y;nextBoundary.y+=increment.y;}
        enter=exitT;
    }
    out_reject=validSeen?6u:5u;
    return false;
}

// Production candidate retains bounded depth iteration; DDA is a separate
// opt-in experiment until native first-hit and image gates pass.
bool scRayCardSurfaceT(SurfaceCacheCard card, vec3 rayO, vec3 rayD,
                       float t_min, float t_max, float tol_band, texture2D depthAtlas,
                       out float out_t, out vec3 out_pos, out vec2 out_atlas_uv, out uint out_reject) {
    out_t=0.;out_pos=vec3(0);out_atlas_uv=vec2(0);out_reject=0u;
    if(any(isnan(rayO)) || any(isinf(rayO)) || any(isnan(rayD)) || any(isinf(rayD)) ||
       any(isnan(card.center)) || any(isinf(card.center)) || any(isnan(card.extent)) || any(isinf(card.extent)) || card.resolution==0u) {
        out_reject=6u;return false;
    }
    vec3 fwd=scCardFaceNormal(card);float dn=dot(rayD,fwd);
    if(abs(dn)<1e-5){out_reject=1u;return false;}
    if(dn<=0.){out_reject=2u;return false;}
    float plane=dot(card.center.xyz-rayO,fwd);
    float t=clamp(plane/dn,t_min,t_max);
    float footprint=length(card.extent.xyz)/float(card.resolution);
    bool converged=false;
    for(uint iteration=0u;iteration<8u;++iteration) {
        vec2 uv;
        if(!worldToCardUVMargin(rayO+rayD*t,card,uv,0.)){out_reject=4u;return false;}
        ivec2 texel=ivec2(clamp(uv,vec2(card.atlas_offset_x,card.atlas_offset_y),vec2(card.atlas_offset_x+card.resolution-1u,card.atlas_offset_y+card.resolution-1u)));
        float depth=texelFetch(depthAtlas,texel,0).r;
        if(isnan(depth)||isinf(depth)||depth<0.){out_reject=5u;return false;}
        float nextT=(plane+depth)/dn;
        if(nextT<t_min||nextT>t_max){out_reject=7u;return false;}
        if(abs(nextT-t)<=max(1e-4,footprint*.25)){t=nextT;converged=true;break;}
        t=nextT;
    }
    if(!converged){out_reject=6u;return false;}
    vec2 uv;
    if(!worldToCardUVMargin(rayO+rayD*t,card,uv,0.)){out_reject=4u;return false;}
    uvec2 texel=uvec2(clamp(uv,vec2(card.atlas_offset_x,card.atlas_offset_y),vec2(card.atlas_offset_x+card.resolution-1u,card.atlas_offset_y+card.resolution-1u)));
    float depth=texelFetch(depthAtlas,ivec2(texel),0).r;
    if(isnan(depth)||isinf(depth)||depth<0.){out_reject=5u;return false;}
    if(abs(depth-(dn*t-plane))>max(1e-4,footprint*.25)){out_reject=6u;return false;}
    vec3 p=cardTexelToWorld(texel,depth,card);
    if(length(p-(rayO+rayD*t))>max(footprint*1.5,1e-4)){out_reject=8u;return false;}
    out_t=t;out_pos=p;out_atlas_uv=vec2(texel)+.5;return true;
}

// 射线 × AABB slab（lookups 级 broad phase，§6.1）。命中 → [t0,t1]。
bool scRayAabbInterval(vec3 rayO, vec3 invD, vec3 bmin, vec3 bmax,
                       float t_min, float t_max, out float t0, out float t1)
{
    t0 = t_min; t1 = t_max;
    for (uint a = 0u; a < 3u; ++a) {
        if (isinf(invD[a])) {
            if (rayO[a] < bmin[a] || rayO[a] > bmax[a]) return false;
        } else {
            float ta = (bmin[a] - rayO[a]) * invD[a];
            float tb = (bmax[a] - rayO[a]) * invD[a];
            t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb));
        }
    }
    return t0 <= t1;
}

#endif // SURFACE_CACHE_DATA_GLSL
