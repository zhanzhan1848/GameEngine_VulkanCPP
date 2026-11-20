#include "DawnSponzaShadowFit.h"
#include <algorithm>
#include <cmath>

namespace primal::tests::dawn_sponza {
// Same scene centroid, light-space texel snapping and sphere depth fit as the
// frozen ShadowMapModule::ComputeCascadeVP. Sponza vertices are world-space.
void FitShadowCascades(ShadowParameters& p,const std::vector<ShadowBounds>& bounds,u32 mapSize) {
    const auto direction=smath::Normalize(smath::v3{p.lightDirection.x,p.lightDirection.y,p.lightDirection.z});
    // ShadowMapModule uses this normalized direction for both fitting and filtering.
    p.lightDirection={direction.x,direction.y,direction.z,p.lightDirection.w};
    const smath::v3 up=std::abs(direction.y)<.99f?smath::v3{0,1,0}:smath::v3{1,0,0};
    smath::v3 center{};
    for(const auto& b:bounds) center=center+b.center;
    if(!bounds.empty())center=center/float(bounds.size());
    auto basis=smath::CreateLookAtMatrix({0,0,0},direction,up);
    const auto inverse=smath::Inverse(basis);
    const float extents[]{12,30,90,270};
    for(u32 c=0;c<4;++c){
        const float texel=2*extents[c]/float(mapSize);
        auto ls=basis*smath::v4{center.x,center.y,center.z,1};
        ls.x=std::floor(ls.x/texel)*texel;ls.y=std::floor(ls.y/texel)*texel;
        const auto snapped=inverse*ls; const smath::v3 target{snapped.x,snapped.y,snapped.z};
        const auto eye=target-direction*200.f;
        float nearDistance=1e10f,farDistance=0;
        for(const auto& b:bounds){
            const auto delta=b.center-eye;
            const float distance=delta.x*direction.x+delta.y*direction.y+delta.z*direction.z;
            nearDistance=std::min(nearDistance,distance-b.radius);farDistance=std::max(farDistance,distance+b.radius);
        }
        const float margin=(farDistance-nearDistance)*.1f;
        const float nearPlane=std::max(nearDistance-margin,.5f),farPlane=farDistance+margin;
        p.lightVP[c]=smath::CreateOrthographicMatrix(-extents[c],extents[c],-extents[c],extents[c],nearPlane,farPlane)
            *smath::CreateLookAtMatrix(eye,target,up);
        p.cascade[c]={texel,farPlane-nearPlane,extents[c],0};
    }
    p.texelSize={1.f/mapSize,1.f/mapSize};
}
}
