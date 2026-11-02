// 48B nodes/triangles, 64B instances; mirrors GeometryFirstHit.h.
struct GeometryNode { vec4 lo; vec4 hi; uvec4 links; };
struct GeometryTriangle { vec4 a; vec4 e1; vec4 e2; };
struct GeometryInstance { vec4 inverse0; vec4 inverse1; vec4 inverse2; uvec4 identity; };
layout(set=0,binding=13,std430) readonly buffer GeometryTop { GeometryNode data[]; } geometryTop;
layout(set=0,binding=14,std430) readonly buffer GeometryBottom { GeometryNode data[]; } geometryBottom;
layout(set=0,binding=15,std430) readonly buffer GeometryTriangles { GeometryTriangle data[]; } geometryTriangles;
layout(set=0,binding=16,std430) readonly buffer GeometryInstances { GeometryInstance data[]; } geometryInstances;
#ifdef DDGI_INSTANCE_NORMAL_CACHE
layout(set=0,binding=28,std430) readonly buffer InstanceNormalOwners { uint data[]; } instanceNormalOwners;
layout(set=0,binding=29,std430) readonly buffer InstanceNormals { vec4 data[]; } instanceNormals;
layout(set=0,binding=30,std430) readonly buffer InstanceNormalVerified { uint data[]; } instanceNormalVerified;
#endif
#ifdef DDGI_GEOMETRY_EQUAL_BOX
#if !defined(DDGI_GEOMETRY_ANY_HIT) || !defined(DDGI_GEOMETRY_REPLACE) || !defined(DDGI_GEOMETRY_QUERY_KERNEL) || defined(DDGI_GEOMETRY_COMPACT_STACKS) || defined(DDGI_GEOMETRY_COMPACT_PACKED) || defined(DDGI_GEOMETRY_BOUNDS_FIRST)
#error Equal-box reuse requires the unmodified fixed-capacity any-hit query path
#endif
layout(set=0,binding=28,std430) readonly buffer GeometryEqualBoxFlags { uint data[]; } geometryEqualBoxFlags;
void geometrySetSkip(inout uvec2 bits,uint index,bool value) {
    uint mask=1u<<(index&31u);uint lane=index>>5u;
    if(value)bits[lane]|=mask;else bits[lane]&=~mask;
}
bool geometryGetSkip(uvec2 bits,uint index) {return (bits[index>>5u] & (1u<<(index&31u)))!=0u;}
uint geometryEdgeFlags(uint index) {return index<geometryEqualBoxFlags.data.length()?geometryEqualBoxFlags.data[index]&3u:0u;}
#endif
#ifdef DDGI_INSTANCE_PAYLOAD_CACHE
#if !defined(DDGI_GEOMETRY_ANY_HIT) || !defined(DDGI_GEOMETRY_REPLACE) || !defined(DDGI_GEOMETRY_QUERY_KERNEL) || defined(DDGI_GEOMETRY_EQUAL_BOX) || defined(DDGI_GEOMETRY_COMPACT_STACKS) || defined(DDGI_GEOMETRY_BOUNDS_FIRST) || defined(DDGI_INSTANCE_NORMAL_CACHE)
#error Instance payload cache requires the original unmodified any-hit query path
#endif
struct GeometryPayloadEntry {GeometryInstance payload;uint key;uint rank;bool valid;};
GeometryPayloadEntry payload0,payload1,payload2,payload3;
void geometryPayloadReset(){payload0.valid=false;payload1.valid=false;payload2.valid=false;payload3.valid=false;}
GeometryInstance geometryPayloadGet(uint key){
    uint entry=4u,oldRank=4u;
    if(payload0.valid && payload0.key==key){entry=0u;oldRank=payload0.rank;}
    else if(payload1.valid && payload1.key==key){entry=1u;oldRank=payload1.rank;}
    else if(payload2.valid && payload2.key==key){entry=2u;oldRank=payload2.rank;}
    else if(payload3.valid && payload3.key==key){entry=3u;oldRank=payload3.rank;}
    GeometryInstance result;
    if(entry==0u)result=payload0.payload;
    else if(entry==1u)result=payload1.payload;
    else if(entry==2u)result=payload2.payload;
    else if(entry==3u)result=payload3.payload;
    else {
#ifdef DDGI_INSTANCE_PAYLOAD_REPLAY
        ++payloadReplayLoads;
#endif
        result=geometryInstances.data[key];
        if(!payload0.valid)entry=0u;else if(!payload1.valid)entry=1u;else if(!payload2.valid)entry=2u;else if(!payload3.valid)entry=3u;
        else if(payload0.rank==3u)entry=0u;else if(payload1.rank==3u)entry=1u;else if(payload2.rank==3u)entry=2u;else entry=3u;
    }
#ifdef DDGI_INSTANCE_PAYLOAD_REPLAY
    if(oldRank!=4u)++payloadReplayHits;
#endif
    if(payload0.valid && payload0.rank<oldRank)++payload0.rank;
    if(payload1.valid && payload1.rank<oldRank)++payload1.rank;
    if(payload2.valid && payload2.rank<oldRank)++payload2.rank;
    if(payload3.valid && payload3.rank<oldRank)++payload3.rank;
    if(entry==0u){payload0.payload=result;payload0.key=key;payload0.rank=0u;payload0.valid=true;}
    else if(entry==1u){payload1.payload=result;payload1.key=key;payload1.rank=0u;payload1.valid=true;}
    else if(entry==2u){payload2.payload=result;payload2.key=key;payload2.rank=0u;payload2.valid=true;}
    else {payload3.payload=result;payload3.key=key;payload3.rank=0u;payload3.valid=true;}
    return result;
}
#endif
// Compensated difference of products prevents skinny-triangle normals from
// losing their small component. No shaderFloat64 capability is required.
float geometryDifferenceOfProducts(float a,float b,float c,float d) {
    precise float cd=c*d;
    precise float error=fma(-c,d,cd);
    precise float difference=fma(a,b,-cd);
    return difference+error;
}
vec3 geometryTriangleNormal(vec3 a,vec3 b) {
    return vec3(geometryDifferenceOfProducts(a.y,b.z,a.z,b.y),
                geometryDifferenceOfProducts(a.z,b.x,a.x,b.z),
                geometryDifferenceOfProducts(a.x,b.y,a.y,b.x));
}
bool geometryBox(GeometryNode n,vec3 o,vec3 d,float nearT,float farT) {
    for(uint k=0u;k<3u;++k) {
        if(d[k]==0.) { if(o[k]<n.lo[k] || o[k]>n.hi[k]) return false; }
        else { float a=(n.lo[k]-o[k])/d[k], b=(n.hi[k]-o[k])/d[k]; nearT=max(nearT,min(a,b)); farT=min(farT,max(a,b)); if(nearT>farT) return false; }
    } return true;
}
#ifndef SC_GEO_NONZERO_ONLY
#define SC_GEO_NONZERO_ONLY false
#endif
// This is exactly the original nonzero-axis branch, in the same axis order.
// Only finite directions with all three components nonzero use it.
bool geometryBoxNonzero(GeometryNode n,vec3 o,vec3 d,float nearT,float farT) {
    for(uint k=0u;k<3u;++k) {
        float a=(n.lo[k]-o[k])/d[k], b=(n.hi[k]-o[k])/d[k];
        nearT=max(nearT,min(a,b)); farT=min(farT,max(a,b)); if(nearT>farT)return false;
    } return true;
}
#ifdef DDGI_GEOMETRY_BOUNDS_FIRST
bool geometryBoxBounds(vec4 low,vec4 high,vec3 o,vec3 d,float nearT,float farT) {
    for(uint k=0u;k<3u;++k) {
        if(d[k]==0.) { if(o[k]<low[k] || o[k]>high[k]) return false; }
        else { float a=(low[k]-o[k])/d[k], b=(high[k]-o[k])/d[k]; nearT=max(nearT,min(a,b)); farT=min(farT,max(a,b)); if(nearT>farT) return false; }
    } return true;
}
bool geometryBoxBoundsNonzero(vec4 low,vec4 high,vec3 o,vec3 d,float nearT,float farT) {
    for(uint k=0u;k<3u;++k) {
        float a=(low[k]-o[k])/d[k], b=(high[k]-o[k])/d[k];
        nearT=max(nearT,min(a,b)); farT=min(farT,max(a,b)); if(nearT>farT)return false;
    } return true;
}
#endif
#ifdef DDGI_TRAVERSAL_AUDIT
int auditTraversalUnknown(uint flag) { auditFlags|=flag; return -1; }
#define GEO_UNKNOWN(flag) return auditTraversalUnknown(flag)
#else
#define GEO_UNKNOWN(flag) return -1
#endif
// -1 unknown, 0 proven segment miss, 1 first opaque two-sided triangle.
int geometryFirstHit(vec3 o,vec3 d,float nearT,float farT,out float bestT,out uint owner,out uint bestPrimitive) {
    bool topNonzero=SC_GEO_NONZERO_ONLY && all(notEqual(d,vec3(0))) && !any(isnan(d)) && !any(isinf(d));
    bestT=farT; owner=0u; bestPrimitive=0xffffffffu; bool found=false;
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
    uint stack[64];
#define ts stack
#elif defined(DDGI_GEOMETRY_COMPACT_STACKS)
    uint ts[16];
#else
    uint ts[64];
#endif
    uint tp=1u; ts[0]=0u;
#ifdef DDGI_GEOMETRY_EQUAL_BOX
    uvec2 topSkipBits=uvec2(0u);
#endif
    while(tp!=0u) {
        uint at=ts[--tp];
#ifdef DDGI_EQUAL_BOX_REPLAY
        ++replayTop;
#endif
        if(at>=geometryTop.data.length()) GEO_UNKNOWN(2u);
#ifdef DDGI_GEOMETRY_EQUAL_BOX
        bool skipTopBox=geometryGetSkip(topSkipBits,tp);
#ifdef DDGI_EQUAL_BOX_REPLAY
        if(skipTopBox)++replaySkipTop;
#endif
#endif
#ifdef DDGI_GEOMETRY_BOUNDS_FIRST
        vec4 topLo=geometryTop.data[at].lo,topHi=geometryTop.data[at].hi;bool topHit;
        if(topNonzero)topHit=geometryBoxBoundsNonzero(topLo,topHi,o,d,nearT,bestT);else topHit=geometryBoxBounds(topLo,topHi,o,d,nearT,bestT);
        if(!topHit)continue;
        GeometryNode n;n.links=geometryTop.data[at].links;
#else
#ifdef DDGI_GEOMETRY_EQUAL_BOX
        GeometryNode n;bool topHit;
        if(skipTopBox){n.links=geometryTop.data[at].links;topHit=true;}
        else {n=geometryTop.data[at];if(topNonzero)topHit=geometryBoxNonzero(n,o,d,nearT,bestT);else topHit=geometryBox(n,o,d,nearT,bestT);}
#else
        GeometryNode n=geometryTop.data[at];bool topHit;
#ifdef DDGI_TRAVERSAL_AUDIT
        auditInc(auditTop);
#endif
        if(topNonzero)topHit=geometryBoxNonzero(n,o,d,nearT,bestT);else topHit=geometryBox(n,o,d,nearT,bestT);
#endif
        if(!topHit)continue;
#endif
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
        if(n.links.w==0u) { if(tp+2u>16u) GEO_UNKNOWN(4u); ts[tp++]=n.links.x; ts[tp++]=n.links.y; continue; }
#else
        if(n.links.w==0u) { if(tp+2u>ts.length()) GEO_UNKNOWN(4u);
#ifdef DDGI_GEOMETRY_EQUAL_BOX
#ifdef DDGI_EQUAL_BOX_REPLAY
            ++replayMetadata;
#endif
            uint f=geometryEdgeFlags(at);geometrySetSkip(topSkipBits,tp,(f&1u)!=0u);geometrySetSkip(topSkipBits,tp+1u,(f&2u)!=0u);
#endif
            ts[tp++]=n.links.x; ts[tp++]=n.links.y; continue; }
#endif
        if(n.links.z>=geometryInstances.data.length()) GEO_UNKNOWN(2u);
#ifdef DDGI_TRAVERSAL_AUDIT
        auditInc(auditInstances);
#endif
#ifdef DDGI_EQUAL_BOX_REPLAY
        ++replayInstances;
#endif
#ifdef DDGI_INSTANCE_PAYLOAD_REPLAY
        ++payloadReplayVisits;
#endif
#ifdef DDGI_INSTANCE_PAYLOAD_CACHE
        GeometryInstance inst=geometryPayloadGet(n.links.z);
#else
#ifdef DDGI_INSTANCE_PAYLOAD_REPLAY
        ++payloadReplayLoads;
#endif
        GeometryInstance inst=geometryInstances.data[n.links.z];
#endif
        vec3 lo=vec3(dot(inst.inverse0,vec4(o,1)),dot(inst.inverse1,vec4(o,1)),dot(inst.inverse2,vec4(o,1)));
        // Do not normalize: inverse scaling must preserve world ray parameter.
        vec3 ld=vec3(dot(inst.inverse0.xyz,d),dot(inst.inverse1.xyz,d),dot(inst.inverse2.xyz,d));
        if(any(isnan(lo))||any(isinf(lo))||any(isnan(ld))||any(isinf(ld))) GEO_UNKNOWN(8u);
        bool bottomNonzero=SC_GEO_NONZERO_ONLY && all(notEqual(ld,vec3(0)));
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
        uint bp=1u; stack[16u]=inst.identity.x;
#else
#ifdef DDGI_GEOMETRY_COMPACT_STACKS
        uint bs[32];
#else
        uint bs[64];
#endif
        uint bp=1u; bs[0]=inst.identity.x;
#endif
#ifdef DDGI_GEOMETRY_EQUAL_BOX
        uvec2 bottomSkipBits=uvec2(0u);
#endif
        while(bp!=0u) {
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
            at=stack[16u + --bp];
#else
            at=bs[--bp];
#endif
#ifdef DDGI_EQUAL_BOX_REPLAY
            ++replayBottom;
#endif
            if(at>=geometryBottom.data.length()) GEO_UNKNOWN(2u);
#ifdef DDGI_GEOMETRY_EQUAL_BOX
            bool skipBottomBox=geometryGetSkip(bottomSkipBits,bp);
#ifdef DDGI_EQUAL_BOX_REPLAY
            if(skipBottomBox)++replaySkipBottom;
#endif
#endif
#ifdef DDGI_GEOMETRY_BOUNDS_FIRST
            vec4 bottomLo=geometryBottom.data[at].lo,bottomHi=geometryBottom.data[at].hi;bool bottomHit;
            if(bottomNonzero)bottomHit=geometryBoxBoundsNonzero(bottomLo,bottomHi,lo,ld,nearT,bestT);else bottomHit=geometryBoxBounds(bottomLo,bottomHi,lo,ld,nearT,bestT);
            if(!bottomHit)continue;
            GeometryNode b;b.links=geometryBottom.data[at].links;
#else
#ifdef DDGI_GEOMETRY_EQUAL_BOX
            GeometryNode b;bool bottomHit;
            if(skipBottomBox){b.links=geometryBottom.data[at].links;bottomHit=true;}
            else {b=geometryBottom.data[at];if(bottomNonzero)bottomHit=geometryBoxNonzero(b,lo,ld,nearT,bestT);else bottomHit=geometryBox(b,lo,ld,nearT,bestT);}
#else
            GeometryNode b=geometryBottom.data[at];bool bottomHit;
#ifdef DDGI_TRAVERSAL_AUDIT
            auditInc(auditBottom);
#endif
            if(bottomNonzero)bottomHit=geometryBoxNonzero(b,lo,ld,nearT,bestT);else bottomHit=geometryBox(b,lo,ld,nearT,bestT);
#endif
            if(!bottomHit)continue;
#endif
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
            if(b.links.w==0u) { if(bp+2u>32u) GEO_UNKNOWN(4u); stack[16u+bp++]=b.links.x; stack[16u+bp++]=b.links.y; continue; }
#else
            if(b.links.w==0u) { if(bp+2u>bs.length()) GEO_UNKNOWN(4u);
#ifdef DDGI_GEOMETRY_EQUAL_BOX
#ifdef DDGI_EQUAL_BOX_REPLAY
                ++replayMetadata;
#endif
                uint f=geometryEdgeFlags(geometryTop.data.length()+at);geometrySetSkip(bottomSkipBits,bp,(f&1u)!=0u);geometrySetSkip(bottomSkipBits,bp+1u,(f&2u)!=0u);
#endif
                bs[bp++]=b.links.x; bs[bp++]=b.links.y; continue; }
#endif
            if(b.links.z>geometryTriangles.data.length() || b.links.w>geometryTriangles.data.length()-b.links.z) GEO_UNKNOWN(2u);
            for(uint i=0u;i<b.links.w;++i) {
#ifdef DDGI_TRAVERSAL_AUDIT
                auditInc(auditTriangles);
#endif
#ifdef DDGI_EQUAL_BOX_REPLAY
                ++replayTriangles;
#endif
                GeometryTriangle tri=geometryTriangles.data[b.links.z+i];
                vec3 p=cross(ld,tri.e2.xyz); float det=dot(tri.e1.xyz,p);
#ifdef DDGI_GEOMETRY_REPLACE
                // An intersected BLAS leaf containing a degenerate or almost
                // parallel triangle cannot certify an open segment. Compute
                // angular comparison in world space: ld is unnormalised so
                // the ray parameter remains world metres under transforms.
#ifdef DDGI_INSTANCE_NORMAL_CACHE
                uint normalIndex=b.links.z+i;
                bool normalCached=normalIndex<instanceNormalOwners.data.length() && normalIndex<instanceNormals.data.length() &&
                    normalIndex<instanceNormalVerified.data.length() && instanceNormalOwners.data[normalIndex]==n.links.z &&
                    instanceNormalVerified.data[normalIndex]==1u;
                vec3 localN; vec3 worldN; float worldNLength;
                if(normalCached) {
                    vec4 cached=instanceNormals.data[normalIndex];
                    localN=cached.xyz; worldN=vec3(0); worldNLength=cached.w;
                } else {
                    localN=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                    worldN=inst.inverse0.xyz*localN.x + inst.inverse1.xyz*localN.y + inst.inverse2.xyz*localN.z;
                    worldNLength=length(worldN);
                }
#else
                vec3 localN=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                vec3 worldN=inst.inverse0.xyz*localN.x + inst.inverse1.xyz*localN.y + inst.inverse2.xyz*localN.z;
                float worldNLength=length(worldN);
#endif
                float facing=dot(ld,localN);
                if(any(isnan(localN))||any(isinf(localN))||any(isnan(worldN))||any(isinf(worldN)) ||
                   isnan(worldNLength)||isinf(worldNLength)||!(worldNLength>0.) ||
                   isnan(facing)||isinf(facing) || isnan(det)||isinf(det) || det==0. ||
                   abs(facing)<=1e-6*worldNLength) GEO_UNKNOWN(8u);
#endif
                if(det==0.) continue;
                vec3 v=lo-tri.a.xyz; float u=dot(v,p)/det;
                if(u<0. || u>1.) continue;
                vec3 q=cross(v,tri.e1.xyz); float bary=dot(ld,q)/det;
                // Barycentrics decide containment; solve t from the plane.
                // The triple product e2 dot ((origin-a) cross e1) suffers
                // cancellation on long thin triangles even for interior hits.
#if defined(DDGI_GEOMETRY_QUERY_KERNEL) && defined(DDGI_GEOMETRY_REPLACE)
                vec3 normal=localN;
#else
                vec3 normal=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
#endif
                float planeDet=dot(ld,normal);
                if(planeDet==0.) GEO_UNKNOWN(8u);
                float t=dot(tri.a.xyz-lo,normal)/planeDet;
                if(isnan(t)||isinf(t)||isnan(bary)||isinf(bary)||isnan(u)||isinf(u)) GEO_UNKNOWN(8u);
                if(bary<0. || u+bary>1. || t<nearT || t>bestT) continue;
                uint primitive=floatBitsToUint(tri.a.w);
#if defined(DDGI_GEOMETRY_ANY_HIT) && defined(DDGI_GEOMETRY_REPLACE)
                // Only OPEN versus non-OPEN is consumed by this variant.
                bestT=t; owner=inst.identity.y; bestPrimitive=primitive;
                return 1;
#endif
                if(found && t==bestT && (inst.identity.y>owner || (inst.identity.y==owner && primitive>=bestPrimitive))) continue;
                owner=inst.identity.y; bestPrimitive=primitive; bestT=t; found=true;
            }
        }
    } return found?1:0;
}
#ifdef DDGI_GEOMETRY_COMPACT_PACKED
#undef ts
#endif

#undef GEO_UNKNOWN
