#pragma once
#include "BoundaryPointIndex.h"
#include "Engine/Graphics/RHI/Core/CardBoundarySidecar.h"
#include <cmath>
#include <cstddef>
namespace primal::graphics::lumen {
// Shared bound for point packing, mixed lookup, current-frame cache and
// 32-ray radiosity resources. No stage may accept a bank another rejects.
inline constexpr size_t BoundaryMaxLightPoints=131072;
// Internal diagnostic GPU mirror, not a persisted format or public ABI.
struct BoundaryLightPoint {
    float world_footprint[4]{},normal[4]{},albedo[4]{};
    uint32_t identity[4]{};
};
static_assert(sizeof(BoundaryLightPoint)==64);
static_assert(offsetof(BoundaryLightPoint,normal)==16 && offsetof(BoundaryLightPoint,identity)==48);
struct BoundaryLightResult {
    float radiance[4],irradiance[4],direct_visibility[4];
    uint32_t identity[4];
};
static_assert(sizeof(BoundaryLightResult)==64);
// Column-major affine transform; inverse transpose normal, then same facing
// convention as StaticComposite. Domain/side filtering keeps each card bank
// independent. Transactional append rejects overflow rather than truncating.
template<class Card,class Matrix>
bool AppendBoundaryLightPoints(const rhi::card_boundary::Mesh& mesh,const Card& card,
                              const Matrix& matrix,uint32_t ci,uint32_t epoch,uint32_t frame,
                              std::vector<BoundaryLightPoint>& out) {
    constexpr size_t MaxPoints=BoundaryMaxLightPoints;
    if(card.resolution==0 || (card.axis_direction&255u)>2 || (card.axis_direction>>8)>1)return false;
    const auto& c=matrix.columns;
    const float values[]={c[0].x,c[0].y,c[0].z,c[0].w,c[1].x,c[1].y,c[1].z,c[1].w,
        c[2].x,c[2].y,c[2].z,c[2].w,c[3].x,c[3].y,c[3].z,c[3].w,
        card.center.x,card.center.y,card.center.z,card.extent.x,card.extent.y,card.extent.z};
    for(const auto& v:values)if(!BoundaryPointIndex::Finite(v))return false;
    if(c[0].w!=0 || c[1].w!=0 || c[2].w!=0 || c[3].w!=1)return false;
    const double m[3][3]={{c[0].x,c[1].x,c[2].x},{c[0].y,c[1].y,c[2].y},{c[0].z,c[1].z,c[2].z}};
    double cof[3][3];
    for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j){const size_t a=(i+1)%3,b=(i+2)%3,d=(j+1)%3,e=(j+2)%3;
        cof[i][j]=m[a][d]*m[b][e]-m[a][e]*m[b][d];}
    const double det=m[0][0]*cof[0][0]+m[0][1]*cof[0][1]+m[0][2]*cof[0][2];
    if(std::abs(det)<1e-12)return false;
    const uint32_t axis=card.axis_direction&255u,u=axis==0?1:0,v=axis==2?1:2;
    const float center[]={card.center.x,card.center.y,card.center.z},extent[]={card.extent.x,card.extent.y,card.extent.z};
    if(extent[u]<=0 || extent[v]<=0)return false;
    const float sign=(card.axis_direction>>8)?1.f:-1.f;
    const float footprint=float(std::sqrt(double(extent[0])*extent[0]+double(extent[1])*extent[1]+double(extent[2])*extent[2])/card.resolution);
    if(!BoundaryPointIndex::Finite(footprint) || footprint<=0)return false;
    std::vector<BoundaryLightPoint> bank;
    for(uint32_t source=0;source<mesh.samples.size();++source){const auto& s=mesh.samples[source];BoundaryLightPoint p;
        if(s.triangle_id>=mesh.triangle_count || s.albedo[3]!=255)return false;
        for(const auto& x:s.position)if(!BoundaryPointIndex::Finite(x))return false;
        for(const auto& x:s.normal)if(!BoundaryPointIndex::Finite(x))return false;
        const float translation[]={c[3].x,c[3].y,c[3].z};double normal[3]{};
        for(size_t k=0;k<3;++k){double position=translation[k];
            for(size_t j=0;j<3;++j){position+=m[k][j]*s.position[j];normal[k]+=cof[k][j]*s.normal[j]/det;}
            p.world_footprint[k]=float(position);if(!BoundaryPointIndex::Finite(p.world_footprint[k]))return false;}
        const double uvU=(double(p.world_footprint[u])-center[u])/(2*extent[u])+.5;
        const double uvV=(double(p.world_footprint[v])-center[v])/(2*extent[v])+.5;
        if(uvU<0 || uvU>=1 || uvV<0 || uvV>=1 || (p.world_footprint[axis]-center[axis])*sign < -1e-5f)continue;
        const double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        if(length<=1e-12)return false;
        const double flip=normal[axis]*sign>0?-1:1;
        for(size_t k=0;k<3;++k){p.normal[k]=float(normal[k]/length*flip);if(!BoundaryPointIndex::Finite(p.normal[k]))return false;p.albedo[k]=float(s.albedo[k])/255;}
        p.normal[3]=1;p.world_footprint[3]=footprint;p.albedo[3]=std::bit_cast<float>(source);
        p.identity[0]=ci;p.identity[1]=s.triangle_id;p.identity[2]=epoch;p.identity[3]=frame;
        if(out.size()+bank.size()>=MaxPoints)return false;
        bank.push_back(p);
    }
    out.insert(out.end(),bank.begin(),bank.end());return true;
}
}
