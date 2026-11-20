// Native first-surface query for the GeometryFirstHit.h 48/48/64-byte ABI.
// Mirrors the frozen DDGITraceSDF measured-geometry branch, not the material
// qualification stage. The caller must supply a complete, current scene and
// counts derived from the bound buffer lengths. Compile with fast-math off.
#include <metal_stdlib>
using namespace metal;

struct GeometryNode { float4 lo, hi; uint4 links; };
struct GeometryTriangle { float4 a, e1, e2; };
struct GeometryInstance { float4 inverse0, inverse1, inverse2; uint4 identity; };
struct GeometryRay { float4 origin_near, direction_far; };
struct GeometryQueryParams { uint4 counts; uint4 rays_ready; };
struct GeometryHit {
    float4 distance_normal;
    uint4 identity; // outcome (signed bits), owner, primitive, measured owner+2
    float4 a, e1, e2;
    float4 measured; // distance classification, oriented geometric normal
};
static_assert(sizeof(GeometryNode)==48 && sizeof(GeometryTriangle)==48, "geometry node/triangle ABI");
static_assert(sizeof(GeometryInstance)==64 && sizeof(GeometryHit)==96, "geometry instance/result ABI");

static float geometryDifferenceOfProducts(float a, float b, float c, float d) {
    const float cd = c*d;
    return fma(a,b,-cd) + fma(-c,d,cd);
}
static float3 geometryTriangleNormal(float3 a, float3 b) {
    return float3(geometryDifferenceOfProducts(a.y,b.z,a.z,b.y),
                  geometryDifferenceOfProducts(a.z,b.x,a.x,b.z),
                  geometryDifferenceOfProducts(a.x,b.y,a.y,b.x));
}
static bool geometryBox(GeometryNode node, float3 o, float3 d, float nearT, float farT) {
    for (uint k=0; k<3; ++k) {
        if (d[k]==0.0f) { if (o[k]<node.lo[k] || o[k]>node.hi[k]) return false; }
        else {
            float a=(node.lo[k]-o[k])/d[k], b=(node.hi[k]-o[k])/d[k];
            nearT=max(nearT,min(a,b)); farT=min(farT,max(a,b));
            if (nearT>farT) return false;
        }
    }
    return true;
}
static bool geometryBoundsValid(GeometryNode n) {
    return all(isfinite(n.lo.xyz)) && all(isfinite(n.hi.xyz)) && all(n.lo.xyz<=n.hi.xyz);
}
// -1 unknown, 0 proven segment miss, 1 closest geometric triangle. No material
// skipping: captured surface qualification must inspect this first surface.
static int geometryFirstHit(float3 o, float3 d, float nearT, float farT,
    device const GeometryNode* top, device const GeometryNode* bottom,
    device const GeometryTriangle* triangles, device const GeometryInstance* instances,
    uint4 counts, thread GeometryHit& result) {
    result={}; result.distance_normal.x=farT; result.identity.z=0xffffffffu;
    if (any(counts==uint4(0)) || !all(isfinite(o)) || !all(isfinite(d)) ||
        !isfinite(nearT) || !isfinite(farT) || nearT<0 || farT<nearT || dot(d,d)==0) return -1;
    float bestT=farT; bool found=false;
    uint ts[64], tp=1, topVisited=0; ts[0]=0;
    while (tp!=0) {
        if (topVisited++>=counts.x) return -1;
        uint at=ts[--tp]; if (at>=counts.x) return -1;
        GeometryNode n=top[at]; if (!geometryBoundsValid(n)) return -1;
        if (!geometryBox(n,o,d,nearT,bestT)) continue;
        if (n.links.w==0) {
            if (tp+2>64) return -1;
            ts[tp++]=n.links.x; ts[tp++]=n.links.y; continue;
        }
        if (n.links.z>=counts.w) return -1;
        GeometryInstance inst=instances[n.links.z];
        float3 localO=float3(dot(inst.inverse0,float4(o,1)),dot(inst.inverse1,float4(o,1)),dot(inst.inverse2,float4(o,1)));
        // Never normalize: this preserves world t under inverse scaling.
        float3 localD=float3(dot(inst.inverse0.xyz,d),dot(inst.inverse1.xyz,d),dot(inst.inverse2.xyz,d));
        if (!all(isfinite(localO)) || !all(isfinite(localD))) return -1;
        uint bs[64], bp=1, bottomVisited=0; bs[0]=inst.identity.x;
        while (bp!=0) {
            if (bottomVisited++>=counts.y) return -1;
            at=bs[--bp]; if (at>=counts.y) return -1;
            GeometryNode b=bottom[at]; if (!geometryBoundsValid(b)) return -1;
            if (!geometryBox(b,localO,localD,nearT,bestT)) continue;
            if (b.links.w==0) {
                if (bp+2>64) return -1;
                bs[bp++]=b.links.x; bs[bp++]=b.links.y; continue;
            }
            if (b.links.z>counts.z || b.links.w>counts.z-b.links.z) return -1;
            for (uint i=0; i<b.links.w; ++i) {
                GeometryTriangle tri=triangles[b.links.z+i];
                float3 p=cross(localD,tri.e2.xyz); float det=dot(tri.e1.xyz,p);
                if (det==0) continue;
                float3 v=localO-tri.a.xyz; float u=dot(v,p)/det;
                if (u<0 || u>1) continue;
                float3 q=cross(v,tri.e1.xyz); float bary=dot(localD,q)/det;
                float3 normal=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                float planeDet=dot(localD,normal); if (planeDet==0) return -1;
                float t=dot(tri.a.xyz-localO,normal)/planeDet;
                if (!isfinite(t) || !isfinite(u) || !isfinite(bary)) return -1;
                if (bary<0 || u+bary>1 || t<nearT || t>bestT) continue;
                uint primitive=as_type<uint>(tri.a.w);
                if (found && t==bestT && (inst.identity.y>result.identity.y ||
                    (inst.identity.y==result.identity.y && primitive>=result.identity.z))) continue;
                float3 worldN=float3(dot(float3(inst.inverse0.x,inst.inverse1.x,inst.inverse2.x),normal),
                    dot(float3(inst.inverse0.y,inst.inverse1.y,inst.inverse2.y),normal),
                    dot(float3(inst.inverse0.z,inst.inverse1.z,inst.inverse2.z),normal));
                float scale=max(abs(worldN.x),max(abs(worldN.y),abs(worldN.z)));
                if (!all(isfinite(worldN)) || scale<=0) return -1;
                bestT=t; found=true; result.distance_normal=float4(t,normalize(worldN/scale));
                result.identity.y=inst.identity.y; result.identity.z=primitive;
                result.a=tri.a; result.e1=tri.e1; result.e2=tri.e2;
            }
        }
    }
    return found?1:0;
}
static void geometryClassifyMeasured(GeometryRay ray, int outcome, GeometryNode root, thread GeometryHit& hit) {
    hit.identity.x=as_type<uint>(outcome); hit.identity.w=0;
    hit.measured=float4(-1,0,0,0);
    // Measured DDGI is anchored at the ray origin; a clipped near segment
    // cannot certify that no earlier surface was skipped. Raw query stays valid.
    if (ray.origin_near.w!=0.0f) return;
    if (outcome==1) {
        float t=hit.distance_normal.x; float3 n=hit.distance_normal.yzw;
        if (t>2e-5f && t<ray.direction_far.w-2e-5f && hit.identity.y<=65535u &&
            all(isfinite(n)) && dot(n,n)>0.25f) {
            if (dot(n,ray.direction_far.xyz)>0) n=-n;
            hit.measured=float4(t,n); hit.identity.w=hit.identity.y+2u;
        }
    } else if (outcome==0) {
        bool sceneAhead=geometryBox(root,ray.origin_near.xyz,ray.direction_far.xyz,ray.direction_far.w,MAXFLOAT);
        hit.measured.x=sceneAhead?-3.0f:-2.0f;
    }
}
// Standalone native consumer, also used by GPU contract tests. DDGI host wiring
// is separate: this entry alone does not enable measured GI in the scene.
kernel void geometry_first_hit(device const GeometryNode* top [[buffer(0)]],
    device const GeometryNode* bottom [[buffer(1)]],
    device const GeometryTriangle* triangles [[buffer(2)]],
    device const GeometryInstance* instances [[buffer(3)]],
    device const GeometryRay* rays [[buffer(4)]], device GeometryHit* hits [[buffer(5)]],
    constant GeometryQueryParams& params [[buffer(6)]], uint id [[thread_position_in_grid]]) {
    if (id>=params.rays_ready.x) return;
    GeometryRay ray=rays[id]; GeometryHit hit={};
    int outcome=-1;
    if (params.rays_ready.y==1)
        outcome=geometryFirstHit(ray.origin_near.xyz,ray.direction_far.xyz,ray.origin_near.w,ray.direction_far.w,
            top,bottom,triangles,instances,params.counts,hit);
    // Unknown/unpublished inputs must not touch a scene buffer for classification.
    GeometryNode root={}; if (outcome==0) root=top[0];
    geometryClassifyMeasured(ray,outcome,root,hit); hits[id]=hit;
}
