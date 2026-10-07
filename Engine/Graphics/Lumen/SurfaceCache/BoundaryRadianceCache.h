#pragma once
#include "BoundaryLightingPoints.h"
#include <span>
#include <cstring>
namespace primal::graphics::lumen {
// Internal current-frame cache. No history reuse: a failed/skipped refresh never
// turns the previous frame's measurement into valid light.
struct BoundaryRadianceEntry {
    float radiance[4]{},indirect[4]{};
    uint32_t identity[4]{},state[4]{}; // frame, epoch, valid, rejection bits
};
static_assert(sizeof(BoundaryRadianceEntry)==64);
static_assert(offsetof(BoundaryRadianceEntry,identity)==32 && offsetof(BoundaryRadianceEntry,state)==48);
class BoundaryRadianceCache {
public:
    struct Key {uint64_t allocation{},geometry{},content{};bool operator==(const Key&) const=default;};
    void Clear(){entries_.clear();points_.clear();ready_=false;}
    bool Begin(Key key,uint32_t frame,std::span<const BoundaryLightPoint> points){
        Clear();key_=key;frame_=frame;
        if(points.empty() || points.size()>BoundaryMaxLightPoints || key.allocation>UINT32_MAX || !key.content)return false;
        for(const auto& p:points)if(p.identity[2]!=key.allocation || p.identity[3]!=frame)return false;
        points_.assign(points.begin(),points.end());return true;
    }
    bool Publish(Key key,uint32_t frame,std::span<const BoundaryRadianceEntry> entries){
        ready_=false;entries_.clear();
        if(key!=key_ || frame!=frame_ || points_.empty() || entries.size()!=points_.size())return false;
        for(size_t i=0;i<entries.size();++i){const auto& e=entries[i];
            if(std::memcmp(e.identity,points_[i].identity,sizeof(e.identity)) || e.state[0]!=frame || e.state[1]!=key.allocation || e.state[2]>1)return false;
            if(e.radiance[3]!=float(e.state[2]) || e.indirect[3]!=float(e.state[2]) || (e.state[2] && e.state[3]))return false;
            for(size_t k=0;k<3;++k)if(!BoundaryPointIndex::Finite(e.radiance[k]) || !BoundaryPointIndex::Finite(e.indirect[k]) ||
                e.radiance[k]<0 || e.radiance[k]>64 || e.indirect[k]<0 || e.indirect[k]>e.radiance[k] ||
                (!e.state[2] && (e.radiance[k]!=0 || e.indirect[k]!=0)))return false;
        }
        entries_.assign(entries.begin(),entries.end());ready_=true;return true;
    }
    const BoundaryRadianceEntry* Find(Key key,uint32_t frame,size_t index,uint32_t card,uint32_t triangle) const {
        if(!ready_ || key!=key_ || frame!=frame_ || index>=entries_.size())return nullptr;
        const auto& e=entries_[index];return e.state[2] && e.identity[0]==card && e.identity[1]==triangle?&e:nullptr;
    }
private:
    Key key_{};uint32_t frame_{};bool ready_=false;
    std::vector<BoundaryLightPoint> points_;std::vector<BoundaryRadianceEntry> entries_;
};
}
