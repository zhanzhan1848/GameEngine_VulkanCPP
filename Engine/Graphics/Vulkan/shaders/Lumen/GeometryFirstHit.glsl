// 48B nodes/triangles, 64B instances; mirrors GeometryFirstHit.h.
struct GeometryNode { vec4 lo; vec4 hi; uvec4 links; };
struct GeometryTriangle { vec4 a; vec4 e1; vec4 e2; };
struct GeometryInstance { vec4 inverse0; vec4 inverse1; vec4 inverse2; uvec4 identity; };
layout(set=0,binding=13,std430) readonly buffer GeometryTop { GeometryNode data[]; } geometryTop;
layout(set=0,binding=14,std430) readonly buffer GeometryBottom { GeometryNode data[]; } geometryBottom;
layout(set=0,binding=15,std430) readonly buffer GeometryTriangles { GeometryTriangle data[]; } geometryTriangles;
layout(set=0,binding=16,std430) readonly buffer GeometryInstances { GeometryInstance data[]; } geometryInstances;
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
// -1 unknown, 0 proven segment miss, 1 first opaque two-sided triangle.
int geometryFirstHit(vec3 o,vec3 d,float nearT,float farT,out float bestT,out uint owner,out uint bestPrimitive) {
    bool topNonzero=SC_GEO_NONZERO_ONLY && all(notEqual(d,vec3(0))) && !any(isnan(d)) && !any(isinf(d));
    bestT=farT; owner=0u; bestPrimitive=0xffffffffu; bool found=false;
    uint ts[64]; uint tp=1u; ts[0]=0u;
    while(tp!=0u) {
        uint at=ts[--tp]; if(at>=geometryTop.data.length()) return -1;
        GeometryNode n=geometryTop.data[at];bool topHit;
        if(topNonzero)topHit=geometryBoxNonzero(n,o,d,nearT,bestT);else topHit=geometryBox(n,o,d,nearT,bestT);
        if(!topHit)continue;
        if(n.links.w==0u) { if(tp+2u>64u) return -1; ts[tp++]=n.links.x; ts[tp++]=n.links.y; continue; }
        if(n.links.z>=geometryInstances.data.length()) return -1;
        GeometryInstance inst=geometryInstances.data[n.links.z];
        vec3 lo=vec3(dot(inst.inverse0,vec4(o,1)),dot(inst.inverse1,vec4(o,1)),dot(inst.inverse2,vec4(o,1)));
        // Do not normalize: inverse scaling must preserve world ray parameter.
        vec3 ld=vec3(dot(inst.inverse0.xyz,d),dot(inst.inverse1.xyz,d),dot(inst.inverse2.xyz,d));
        if(any(isnan(lo))||any(isinf(lo))||any(isnan(ld))||any(isinf(ld))) return -1;
        bool bottomNonzero=SC_GEO_NONZERO_ONLY && all(notEqual(ld,vec3(0)));
        uint bs[64]; uint bp=1u; bs[0]=inst.identity.x;
        while(bp!=0u) {
            at=bs[--bp]; if(at>=geometryBottom.data.length()) return -1;
            GeometryNode b=geometryBottom.data[at];bool bottomHit;
            if(bottomNonzero)bottomHit=geometryBoxNonzero(b,lo,ld,nearT,bestT);else bottomHit=geometryBox(b,lo,ld,nearT,bestT);
            if(!bottomHit)continue;
            if(b.links.w==0u) { if(bp+2u>64u) return -1; bs[bp++]=b.links.x; bs[bp++]=b.links.y; continue; }
            if(b.links.z>geometryTriangles.data.length() || b.links.w>geometryTriangles.data.length()-b.links.z) return -1;
            for(uint i=0u;i<b.links.w;++i) {
                GeometryTriangle tri=geometryTriangles.data[b.links.z+i];
                vec3 p=cross(ld,tri.e2.xyz); float det=dot(tri.e1.xyz,p);
                if(det==0.) continue;
                vec3 v=lo-tri.a.xyz; float u=dot(v,p)/det;
                if(u<0. || u>1.) continue;
                vec3 q=cross(v,tri.e1.xyz); float bary=dot(ld,q)/det;
                // Barycentrics decide containment; solve t from the plane.
                // The triple product e2 dot ((origin-a) cross e1) suffers
                // cancellation on long thin triangles even for interior hits.
                vec3 normal=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                float planeDet=dot(ld,normal);
                if(planeDet==0.) return -1;
                float t=dot(tri.a.xyz-lo,normal)/planeDet;
                if(isnan(t)||isinf(t)||isnan(bary)||isinf(bary)||isnan(u)||isinf(u)) return -1;
                if(bary<0. || u+bary>1. || t<nearT || t>bestT) continue;
                uint primitive=floatBitsToUint(tri.a.w);
                if(found && t==bestT && (inst.identity.y>owner || (inst.identity.y==owner && primitive>=bestPrimitive))) continue;
                owner=inst.identity.y; bestPrimitive=primitive; bestT=t; found=true;
            }
        }
    } return found?1:0;
}
