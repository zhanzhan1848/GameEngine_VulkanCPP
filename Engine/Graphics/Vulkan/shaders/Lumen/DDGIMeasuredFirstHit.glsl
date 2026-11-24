// -1 unknown, 0 segment miss, 1 first geometric triangle. Material and
// captured surface qualification are checked separately; never skip it.
int geometryFirstHit(vec3 o,vec3 d,float nearT,float farT,out float bestT,out uint owner,out uint bestPrimitive,out vec3 firstN,out vec3 firstA,out vec3 firstE1,out vec3 firstE2) {
    bool topNonzero=SC_GEO_NONZERO_ONLY && all(notEqual(d,vec3(0))) && !any(isnan(d)) && !any(isinf(d));
    firstN=vec3(0);firstA=vec3(0);firstE1=vec3(0);firstE2=vec3(0);bestT=farT; owner=0u; bestPrimitive=0xffffffffu; bool found=false;
    uint topVisited=0u;
    uint ts[64]; uint tp=1u; ts[0]=0u;
    while(tp!=0u) {
        if(topVisited++>=geometryTop.data.length()) return -1;
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
        uint bottomVisited=0u;
        uint bs[64]; uint bp=1u; bs[0]=inst.identity.x;
        while(bp!=0u) {
            if(bottomVisited++>=geometryBottom.data.length()) return -1;
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
                // Inverse transpose transforms the local geometric normal;
                // the unnormalized local direction preserves WORLD t.
                vec3 worldN=vec3(
                    dot(vec3(inst.inverse0.x,inst.inverse1.x,inst.inverse2.x),normal),
                    dot(vec3(inst.inverse0.y,inst.inverse1.y,inst.inverse2.y),normal),
                    dot(vec3(inst.inverse0.z,inst.inverse1.z,inst.inverse2.z),normal));
                float normalScale=max(abs(worldN.x),max(abs(worldN.y),abs(worldN.z)));
                if(any(isnan(worldN))||any(isinf(worldN))||normalScale<=0.) return -1;
                firstN=normalize(worldN/normalScale);
                firstA=tri.a.xyz;firstE1=tri.e1.xyz;firstE2=tri.e2.xyz;
            }
        }
    } return found?1:0;
}
