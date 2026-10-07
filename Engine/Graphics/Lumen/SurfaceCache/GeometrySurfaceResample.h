#pragma once
#include "Graphics/Utilities/GeometryMeshTrace.h"
#include "Graphics/RHI/Core/CardSurfaceIdentity.h"
#include <cstring>
#include <vector>

namespace primal::graphics::lumen {
struct GeometrySurfacePage {
    std::vector<uint8_t> albedo,normal;
    std::vector<float> depth;
    std::vector<uint32_t> triangles;
    uint32_t valid=0, no_source=0, geometry_miss=0;
};
// Re-rasterize at TARGET texel centers. A source footprint supplies attributes
// only when its TriangleID matches the nearest owner triangle at that center.
// No averaging across primitives, extrapolated depth, or borrowed back layer.
inline bool ResampleGeometrySurface(std::span<const ray_geometry::Node> nodes,
    std::span<const ray_geometry::Triangle> triangles, uint32_t root,
    const float* center,const float* extent,uint32_t axisDirection,
    uint32_t sourceResolution,uint32_t targetResolution,
    std::span<const uint8_t> sourceAlbedo,std::span<const uint8_t> sourceNormal,
    std::span<const uint32_t> sourceTriangles,GeometrySurfacePage& out) {
    out={};const uint32_t axis=axisDirection&255u;
    const uint64_t n=uint64_t(sourceResolution)*sourceResolution;
    if(axis>2 || (axisDirection>>8)>1 || !sourceResolution || !targetResolution || sourceResolution>4096 || targetResolution>4096 ||
       sourceAlbedo.size()!=n*4 || sourceNormal.size()!=n*4 || sourceTriangles.size()!=n || root>=nodes.size())return false;
    const auto finite=[](float f){volatile uint32_t b=std::bit_cast<uint32_t>(f);return (b&0x7f800000u)!=0x7f800000u;};
    for(uint32_t k=0;k<3;++k)if(!finite(center[k])||!finite(extent[k])||extent[k]<=0)return false;
    const uint64_t count=uint64_t(targetResolution)*targetResolution;
    out.albedo.assign(count*4,0);out.normal.assign(count*4,0);
    out.depth.assign(count,-1.f);out.triangles.assign(count,0xffffffffu);
    float direction[3]{};direction[axis]=(axisDirection>>8)?1.f:-1.f;
    const uint32_t u=axis==0?1:0,v=axis==2?1:2;
    const float farT=2.f*extent[axis]*rhi::card_surface_identity::CaptureFarScale+rhi::card_surface_identity::CaptureFarBias;
    for(uint32_t y=0;y<targetResolution;++y)for(uint32_t x=0;x<targetResolution;++x) {
        const uint32_t x0=uint64_t(x)*sourceResolution/targetResolution;
        const uint32_t y0=uint64_t(y)*sourceResolution/targetResolution;
        const uint32_t x1=(uint64_t(x+1)*sourceResolution+targetResolution-1)/targetResolution;
        const uint32_t y1=(uint64_t(y+1)*sourceResolution+targetResolution-1)/targetResolution;
        bool source=false;
        for(uint32_t sy=y0;sy<y1;++sy)for(uint32_t sx=x0;sx<x1;++sx)
            source |= sourceAlbedo[(uint64_t(sy)*sourceResolution+sx)*4+3]>=128;
        if(!source){++out.no_source;continue;}
        float origin[3]{center[0],center[1],center[2]};
        origin[u]+=((float(x)+.5f)/targetResolution-.5f)*2.f*extent[u];
        origin[v]+=((float(y)+.5f)/targetResolution-.5f)*2.f*extent[v];
        ray_geometry::Hit hit;
        const int status=ray_geometry::TraceMesh(nodes,triangles,root,origin,direction,rhi::card_surface_identity::CaptureNear,farT,hit);
        if(status<0){out={};return false;}
        if(status==0){++out.geometry_miss;continue;}
        uint64_t best=n;double bestDistance=1e30;
        const double tx=(double(x)+.5)*sourceResolution/targetResolution,ty=(double(y)+.5)*sourceResolution/targetResolution;
        for(uint32_t sy=y0;sy<y1;++sy)for(uint32_t sx=x0;sx<x1;++sx) {
            const uint64_t index=uint64_t(sy)*sourceResolution+sx;
            if(sourceTriangles[index]!=hit.triangle_id || sourceAlbedo[index*4+3]<128)continue;
            const double dx=sx+.5-tx,dy=sy+.5-ty,distance=dx*dx+dy*dy;
            if(distance<bestDistance){bestDistance=distance;best=index;}
        }
        if(best==n){++out.no_source;continue;}
        const uint64_t at=uint64_t(y)*targetResolution+x;
        std::memcpy(out.albedo.data()+at*4,sourceAlbedo.data()+best*4,4);
        std::memcpy(out.normal.data()+at*4,sourceNormal.data()+best*4,4);
        out.depth[at]=hit.t;out.triangles[at]=hit.triangle_id;++out.valid;
    }
    return true;
}
}
