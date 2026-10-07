#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <span>
#include <set>
#include <vector>
namespace primal::graphics::lumen {
// Epoch-local mixed atlas/auxiliary point index. Caller supplies world positions
// only after resolving instance + geometry/layout/material identity. No radiance
// is stored here; selecting a point does not make it a valid cache hit.
class BoundaryPointIndex {
public:
    static constexpr uint32_t Magic=0x53434b31u, MaxWords=4u*1024u*1024u;
    static constexpr uint32_t MaxCards=65536, MaxGroupPoints=8192;
    struct Point { std::array<float,3> position; uint32_t source; };
    static_assert(sizeof(Point)==16);
    static bool Finite(const float& value) {
        uint32_t bits=std::bit_cast<uint32_t>(value);
        return (bits&0x7f800000u)!=0x7f800000u;
    }
    // Build is transactional; failure clears output rather than leaving stale
    // epoch data. Card + original primitive form the directory key.
    struct Group { uint32_t card, triangle; std::vector<Point> points; };
    static bool Build(uint32_t cards,uint32_t epoch,std::span<const Group> groups,
                      std::vector<uint32_t>& words) {
        words.clear();
        if(cards==0 || cards>MaxCards)return false;
        std::map<std::pair<uint32_t,uint32_t>,const Group*> ordered;
        std::vector<uint32_t> cardGroups(cards,0);
        uint64_t total=0;
        for(const auto& g:groups) {
            if(g.card>=cards || g.triangle==0xffffffffu || g.points.empty() || g.points.size()>MaxGroupPoints ||
               !ordered.emplace(std::pair{g.card,g.triangle},&g).second)return false;
            if(++cardGroups[g.card]>8192)return false;
            total+=g.points.size();
            if(total*4+8+2ull*cards+ordered.size()*4>MaxWords)return false;
            std::set<uint32_t> sources;
            for(const auto& p:g.points){
                if(p.source==0xffffffffu || !sources.insert(p.source).second)return false;
                for(const auto& v:p.position)if(!Finite(v))return false;
            }
        }
        std::vector<uint32_t> nodes,points;
        struct Record {uint32_t card,triangle,root,count;};
        std::vector<Record> records;
        for(const auto& [key,group]:ordered) {
            auto p=group->points;
            auto build=[&](auto&& self,size_t first,size_t end)->uint32_t {
                const uint32_t node=uint32_t(nodes.size());nodes.resize(nodes.size()+12,0);
                auto lo=p[first].position,hi=lo;
                for(size_t i=first+1;i<end;++i)for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],p[i].position[k]);hi[k]=std::max(hi[k],p[i].position[k]);}
                for(size_t k=0;k<3;++k){nodes[node+k]=std::bit_cast<uint32_t>(lo[k]);nodes[node+4+k]=std::bit_cast<uint32_t>(hi[k]);}
                if(end-first<=8) {
                    nodes[node+8]=uint32_t(points.size());nodes[node+9]=uint32_t(end-first);
                    for(size_t i=first;i<end;++i){for(float v:p[i].position)points.push_back(std::bit_cast<uint32_t>(v));points.push_back(p[i].source);}
                } else {
                    size_t axis=0;for(size_t k=1;k<3;++k)if(double(hi[k])-lo[k]>double(hi[axis])-lo[axis])axis=k;
                    const size_t mid=(first+end)/2;
                    std::nth_element(p.begin()+first,p.begin()+mid,p.begin()+end,[axis](const Point& a,const Point& b){
                        return a.position[axis]!=b.position[axis]?a.position[axis]<b.position[axis]:a.source<b.source;});
                    const uint32_t left=self(self,first,mid),right=self(self,mid,end);
                    nodes[node+3]=left;nodes[node+7]=right;
                }
                return node;
            };
            records.push_back({key.first,key.second,build(build,0,p.size()),uint32_t(p.size())});
            if(8+2ull*cards+records.size()*4+nodes.size()+points.size()>MaxWords)return false;
        }
        const uint32_t recordBegin=8+2*cards,nodeBegin=recordBegin+uint32_t(records.size()*4),pointBegin=nodeBegin+uint32_t(nodes.size());
        std::vector<uint32_t> result(recordBegin,0);
        result[0]=Magic;result[1]=epoch;result[2]=cards;result[3]=pointBegin+uint32_t(points.size());
        result[4]=nodeBegin;result[5]=pointBegin;result[6]=uint32_t(nodes.size()/12);result[7]=uint32_t(points.size()/4);
        for(const auto& r:records){auto& offset=result[8+2*r.card];if(!offset)offset=uint32_t(result.size());++result[9+2*r.card];
            result.insert(result.end(),{r.triangle,nodeBegin+r.root,r.count,0});}
        for(size_t i=0;i<nodes.size();i+=12){if(nodes[i+9])nodes[i+8]+=pointBegin;else {nodes[i+3]+=nodeBegin;nodes[i+7]+=nodeBegin;}}
        result.insert(result.end(),nodes.begin(),nodes.end());result.insert(result.end(),points.begin(),points.end());
        words.swap(result);return true;
    }
};
}
