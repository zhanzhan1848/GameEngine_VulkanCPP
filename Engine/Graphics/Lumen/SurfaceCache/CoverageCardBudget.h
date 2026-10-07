#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <queue>
#include <span>
#include <vector>

namespace primal::graphics::lumen {
struct CoverageCardRequest {
    uint32_t pool_index, rank, requested_resolution;
    float projected_area;
    float projected_span=0.f; // Longest world-space projected side; zero derives from area.
};
struct CoverageCardPlacement {
    uint32_t pool_index, resolution, atlas_offset_x, atlas_offset_y;
};
struct CoverageCardPlan {
    std::vector<CoverageCardPlacement> placements;
    uint32_t cursor_x=0, cursor_y=0, row_height=0;
};
// Reserve one minimum page for EVERY candidate, then spend the remaining
// texel budget on rank/area refinement, or experimental world-pitch refinement.
// Power-of-two squares sorted
// largest-first on an aligned page grid avoid mixed-height shelf gaps.
inline bool PlanCoverageCards(std::span<const CoverageCardRequest> requests,
                               uint32_t atlas, uint32_t page, uint32_t max_cards,
                               CoverageCardPlan& out, bool density_refinement=false) {
    out={};
    if(page<32 || !std::has_single_bit(page) || !std::has_single_bit(atlas) || page>atlas ||
       requests.size()>max_cards || uint64_t(requests.size())*page*page>uint64_t(atlas)*atlas) return false;
    if(requests.empty()) return true;
    std::vector<uint32_t> order(requests.size()), resolutions(requests.size(),page), caps(requests.size());
    std::vector<uint32_t> ids;
    for(uint32_t i=0;i<requests.size();++i) {
        const auto& r=requests[i];
        volatile uint32_t bits=std::bit_cast<uint32_t>(r.projected_area);
        if((bits&0x7f800000u)==0x7f800000u || !(r.projected_area>0) || r.requested_resolution<page) return false;
        volatile uint32_t spanBits=std::bit_cast<uint32_t>(r.projected_span);
        if((spanBits&0x7f800000u)==0x7f800000u || r.projected_span<0) return false;
        ids.push_back(r.pool_index); order[i]=i;
        caps[i]=std::bit_floor(std::min(r.requested_resolution,atlas));
    }
    std::sort(ids.begin(),ids.end());
    if(std::adjacent_find(ids.begin(),ids.end())!=ids.end()) return false;
    std::stable_sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b) {
        if(requests[a].rank!=requests[b].rank) return requests[a].rank<requests[b].rank;
        if(requests[a].projected_area!=requests[b].projected_area)
            return requests[a].projected_area>requests[b].projected_area;
        return requests[a].pool_index<requests[b].pool_index;
    });
    uint64_t remaining=uint64_t(atlas)*atlas-uint64_t(requests.size())*page*page;
    uint32_t maximum=*std::max_element(caps.begin(),caps.end());
    if(density_refinement) {
        struct Refinement { double pitch;uint32_t index,resolution; };
        auto less=[&](const Refinement& a,const Refinement& b) {
            if(a.pitch!=b.pitch)return a.pitch<b.pitch;
            if(requests[a.index].rank!=requests[b.index].rank)return requests[a.index].rank>requests[b.index].rank;
            return requests[a.index].pool_index>requests[b.index].pool_index;
        };
        std::priority_queue<Refinement,std::vector<Refinement>,decltype(less)> pending(less);
        const auto span=[&](uint32_t i) {return requests[i].projected_span>0?double(requests[i].projected_span):std::sqrt(double(requests[i].projected_area));};
        for(uint32_t i=0;i<requests.size();++i)if(caps[i]>page)pending.push({span(i)/page,i,page});
        while(!pending.empty()) {
            const auto item=pending.top();pending.pop();
            const uint64_t extra=uint64_t(item.resolution)*item.resolution*3;
            if(extra>remaining)continue; // Remaining budget only decreases.
            remaining-=extra;resolutions[item.index]*=2;
            if(resolutions[item.index]<caps[item.index])pending.push({span(item.index)/resolutions[item.index],item.index,resolutions[item.index]});
        }
    } else for(uint32_t level=page;level<maximum;level*=2) {
        const uint64_t extra=uint64_t(level)*level*3;
        for(uint32_t i:order) if(resolutions[i]==level && caps[i]>=level*2 && remaining>=extra) {
            resolutions[i]*=2; remaining-=extra;
        }
    }
    std::stable_sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b) {return resolutions[a]>resolutions[b];});
    const uint32_t side=atlas/page;
    std::vector<uint8_t> occupied(uint64_t(side)*side,0);
    for(uint32_t i:order) {
        uint32_t res=resolutions[i], width=res/page;
        bool placed=false;
        for(uint32_t y=0;y+width<=side && !placed;y+=width) for(uint32_t x=0;x+width<=side && !placed;x+=width) {
            bool free=true;
            for(uint32_t v=y;v<y+width && free;++v) for(uint32_t u=x;u<x+width;++u)
                if(occupied[uint64_t(v)*side+u]) {free=false;break;}
            if(!free) continue;
            for(uint32_t v=y;v<y+width;++v) for(uint32_t u=x;u<x+width;++u) occupied[uint64_t(v)*side+u]=1;
            out.placements.push_back({requests[i].pool_index,res,x*page,y*page});
            out.cursor_y=std::max(out.cursor_y,(y+width)*page);
            placed=true;
        }
        if(!placed) {out={};return false;}
    }
    // Legacy fallback emit may append only below the packed rectangles.
    // Holes in earlier rows are owned by this grid, not the shelf cursor.
    out.cursor_x=0;out.row_height=0;
    return true;
}
} // namespace primal::graphics::lumen
