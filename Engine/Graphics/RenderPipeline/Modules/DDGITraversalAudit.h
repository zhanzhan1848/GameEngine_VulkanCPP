#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <algorithm>

namespace primal::graphics::traversal_audit {
constexpr uint32_t Magic=0x44544741u, MaxRecords=1024, HeaderWords=16, QueryWords=16, RecordWords=144;
struct Pixel {uint32_t x,y;};
struct alignas(16) Selection {uint32_t dims[4]{},config[4]{},roi[8][4]{};};
static_assert(sizeof(Selection)==160);
inline bool BuildTraversalAuditSelection(uint32_t width,uint32_t height,uint32_t gridW,uint32_t gridH,
    std::span<const Pixel> roi,uint32_t frame,uint32_t slot,Selection& out,std::vector<Pixel>& pixels) {
    out={};pixels.clear(); if(!width||!height||roi.size()>8||slot>2||(gridW==0)!=(gridH==0) ||
        gridW>width||gridH>height || uint64_t(gridW)*gridH>MaxRecords ||
        uint64_t(width)*2u>UINT32_MAX || uint64_t(height)*2u>UINT32_MAX ||
        uint64_t(width)*2u*gridW>UINT32_MAX || uint64_t(height)*2u*gridH>UINT32_MAX) return false;
    out.dims[0]=width;out.dims[1]=height;out.dims[2]=gridW;out.dims[3]=gridH;
    out.config[0]=frame;out.config[1]=slot;
    auto find=[&](Pixel p){return std::find_if(pixels.begin(),pixels.end(),[&](Pixel q){return p.x==q.x&&p.y==q.y;});};
    for(uint32_t y=0;y<gridH;++y)for(uint32_t x=0;x<gridW;++x) {
        Pixel p{uint32_t((uint64_t(2*x+1)*width)/(2*gridW)),uint32_t((uint64_t(2*y+1)*height)/(2*gridH))};
        if(p.x>=width||p.y>=height||find(p)!=pixels.end() || (uint64_t(2*p.x+1)*gridW)/(2*width)!=x ||
           (uint64_t(2*p.y+1)*gridH)/(2*height)!=y){pixels.clear();return false;}
        pixels.push_back(p);
    }
    for(auto p:roi) {
        if(p.x>=width||p.y>=height){pixels.clear();return false;}
        auto it=find(p);if(it!=pixels.end())continue;
        if(pixels.size()>=MaxRecords){pixels.clear();return false;}
        const uint32_t i=out.config[2]++;out.roi[i][0]=p.x;out.roi[i][1]=p.y;out.roi[i][2]=uint32_t(pixels.size());pixels.push_back(p);
    }
    out.config[3]=uint32_t(pixels.size());return !pixels.empty();
}
}
