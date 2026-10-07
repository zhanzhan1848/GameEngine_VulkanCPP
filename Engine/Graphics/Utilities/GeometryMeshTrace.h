#pragma once
#include "GeometryFirstHit.h"
#include <algorithm>
#include <bit>

namespace primal::graphics::ray_geometry {
// Owner-only BLAS query in mesh coordinates. No normalization or TLAS walk.
// 0 miss, 1 closest two-sided primitive, -1 unavailable/invalid/overflow.
inline int TraceMesh(std::span<const Node> nodes, std::span<const Triangle> triangles,
                     u32 root, const float* origin, const float* direction,
                     float nearT, float farT, Hit& hit) {
    const auto finite=[](float f) { volatile u32 bits=std::bit_cast<u32>(f); return (bits&0x7f800000u)!=0x7f800000u; };
    hit={};hit.t=farT;
    if(!finite(nearT)||!finite(farT)||nearT<0||nearT>=farT||root>=nodes.size()) return -1;
    double norm=0;
    for(u32 k=0;k<3;++k) { if(!finite(origin[k])||!finite(direction[k])) return -1;norm+=double(direction[k])*direction[k]; }
    if(norm==0) return -1;
    u32 stack[64]{root},size=1;bool found=false;
    while(size) {
        const u32 at=stack[--size];if(at>=nodes.size()) return -1;
        const auto& n=nodes[at];float lo=nearT,hi=hit.t;bool overlaps=true;
        for(u32 k=0;k<3;++k) {
            if(direction[k]==0) { if(origin[k]<n.lo[k]||origin[k]>n.hi[k]) overlaps=false; }
            else { const float a=(n.lo[k]-origin[k])/direction[k],b=(n.hi[k]-origin[k])/direction[k];
                lo=std::max(lo,std::min(a,b));hi=std::min(hi,std::max(a,b));if(lo>hi) overlaps=false; }
        }
        if(!overlaps) continue;
        if(!n.links[3]) { if(size+2>64) return -1;stack[size++]=n.links[0];stack[size++]=n.links[1];continue; }
        if(n.links[2]>triangles.size() || n.links[3]>triangles.size()-n.links[2]) return -1;
        for(u32 i=0;i<n.links[3];++i) {
            const auto& tri=triangles[n.links[2]+i];double p[3],q[3],v[3];
            for(u32 k=0;k<3;++k) {const u32 a=(k+1)%3,b=(k+2)%3;
                p[k]=double(direction[a])*tri.e2[b]-double(direction[b])*tri.e2[a];v[k]=double(origin[k])-tri.a[k];}
            double det=0,u=0;for(u32 k=0;k<3;++k){det+=tri.e1[k]*p[k];u+=v[k]*p[k];}
            if(det==0)continue;u/=det;if(u<0||u>1)continue;
            for(u32 k=0;k<3;++k){const u32 a=(k+1)%3,b=(k+2)%3;q[k]=v[a]*tri.e1[b]-v[b]*tri.e1[a];}
            double t=0,bary=0;for(u32 k=0;k<3;++k){t+=tri.e2[k]*q[k];bary+=direction[k]*q[k];}
            t/=det;bary/=det;if(bary<0||u+bary>1||t<nearT||t>hit.t)continue;
            if(!finite(float(t))||!finite(float(u))||!finite(float(bary)))return -1;
            const u32 id=std::bit_cast<u32>(tri.a[3]);
            if(found && float(t)==hit.t && id>=hit.triangle_id)continue;
            hit.t=float(t);hit.u=float(u);hit.v=float(bary);hit.triangle_id=id;found=true;
        }
    }
    return found?1:0;
}
}
