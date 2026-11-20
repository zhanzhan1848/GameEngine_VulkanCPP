// Gather connection qualification only; measured first-surface queries keep
// GeometryFirstHit.metal. A traversed near-parallel or degenerate triangle
// cannot certify an open connection (frozen world-space angular threshold).
static int geometryConnectionFirstHit(float3 o, float3 d, float nearT, float farT,
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
                float3 cn=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                float3 wn=inst.inverse0.xyz*cn.x+inst.inverse1.xyz*cn.y+inst.inverse2.xyz*cn.z;
                float len=length(wn),facing=dot(localD,cn);
                if(!all(isfinite(cn))||!all(isfinite(wn))||!isfinite(len)||!(len>0)||!isfinite(facing)||!isfinite(det)||det==0||abs(facing)<=1e-6f*len)return -1;
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
