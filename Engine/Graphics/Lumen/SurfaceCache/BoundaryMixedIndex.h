#pragma once
#include "BoundaryLightingPoints.h"
#include <map>
namespace primal::graphics::lumen {
// Mirror a same-resolution static composite for positive diagonal transforms.
// The GPU consumer still verifies actual ID/depth/position before using a source.
template<class Card,class Matrix>
bool AppendBoundaryAtlasPoints(const Card& card,const Matrix& matrix,uint32_t ci,uint32_t localAxis,
    uint32_t pageResolution,std::span<const uint32_t> triangles,std::span<const float> depths,
    std::span<const uint8_t> albedo,std::vector<BoundaryPointIndex::Group>& output){
    const uint32_t r=card.resolution,axis=card.axis_direction&255u;
    const uint64_t n=uint64_t(r)*r;
    if(!r || r>4096 || pageResolution!=r || axis>2 || (card.axis_direction>>8)>1 || localAxis!=card.axis_direction ||
       triangles.size()!=n || depths.size()!=n || albedo.size()!=n*4 ||
       uint64_t(card.atlas_offset_x)+r>65536 || uint64_t(card.atlas_offset_y)+r>32768)return false;
    const auto& m=matrix.columns;
    const float diagonal[]={m[0].x,m[1].y,m[2].z};
    if(m[0].y!=0 || m[0].z!=0 || m[1].x!=0 || m[1].z!=0 || m[2].x!=0 || m[2].y!=0 ||
       m[0].w!=0 || m[1].w!=0 || m[2].w!=0 || m[3].w!=1)return false;
    const float center[]={card.center.x,card.center.y,card.center.z},extent[]={card.extent.x,card.extent.y,card.extent.z};
    for(uint32_t k=0;k<3;++k)if(!BoundaryPointIndex::Finite(diagonal[k]) || diagonal[k]<=0 ||
        !BoundaryPointIndex::Finite(center[k]) || !BoundaryPointIndex::Finite(extent[k]) || extent[k]<=0)return false;
    const uint32_t u=axis==0?1:0,v=axis==2?1:2;const float sign=(card.axis_direction>>8)?1.f:-1.f;
    std::map<uint32_t,std::vector<BoundaryPointIndex::Point>> groups;
    for(uint32_t y=0;y<r;++y)for(uint32_t x=0;x<r;++x){const size_t at=size_t(y)*r+x;
        if(albedo[at*4+3]<128 || triangles[at]==UINT32_MAX)continue;
        if(!BoundaryPointIndex::Finite(depths[at]) || depths[at]<0)return false;
        // Preserve the composite's add/subtract rounding; fast-math must not
        // cancel center before the GPU depth quantization step.
        volatile float worldAxis=center[axis]+sign*depths[at]*diagonal[axis];
        const float worldDepth=(worldAxis-center[axis])*sign;
        BoundaryPointIndex::Point point{{center[0],center[1],center[2]},
            (card.atlas_offset_x+x)|((card.atlas_offset_y+y)<<16)};
        point.position[axis]+=sign*worldDepth;
        point.position[u]+=((float(x)+.5f)/r-.5f)*extent[u]*2;
        point.position[v]+=((float(y)+.5f)/r-.5f)*extent[v]*2;
        for(float p:point.position)if(!BoundaryPointIndex::Finite(p))return false;
        groups[triangles[at]].push_back(point);
        if(groups[triangles[at]].size()>BoundaryPointIndex::MaxGroupPoints)return false;
    }
    for(auto& [triangle,points]:groups)output.push_back({ci,triangle,std::move(points)});
    return true;
}
inline bool BuildBoundaryMixedIndex(uint32_t cards,uint32_t epoch,
    std::span<const BoundaryPointIndex::Group> atlas,std::span<const BoundaryLightPoint> auxiliary,
    std::vector<uint32_t>& words){
    words.clear();if(auxiliary.empty() || auxiliary.size()>BoundaryMaxLightPoints)return false;
    std::map<std::pair<uint32_t,uint32_t>,BoundaryPointIndex::Group> groups;
    for(uint32_t i=0;i<auxiliary.size();++i){const auto& p=auxiliary[i];
        if(p.identity[0]>=cards || p.identity[1]==UINT32_MAX || p.identity[2]!=epoch)return false;
        const auto key=std::pair{p.identity[0],p.identity[1]};auto& g=groups[key];g.card=key.first;g.triangle=key.second;
        g.points.push_back({{p.world_footprint[0],p.world_footprint[1],p.world_footprint[2]},0x80000000u|i});
    }
    std::set<std::pair<uint32_t,uint32_t>> seen;
    for(const auto& g:atlas){const auto key=std::pair{g.card,g.triangle};if(!seen.insert(key).second)return false;
        if(auto at=groups.find(key);at!=groups.end())at->second.points.insert(at->second.points.end(),g.points.begin(),g.points.end());}
    std::vector<BoundaryPointIndex::Group> merged;for(auto& [key,g]:groups)merged.push_back(std::move(g));
    return BoundaryPointIndex::Build(cards,epoch,merged,words);
}
}
