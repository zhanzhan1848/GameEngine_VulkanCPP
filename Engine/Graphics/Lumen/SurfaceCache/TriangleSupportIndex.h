#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <span>
#include <vector>
namespace primal::graphics::lumen {
// Internal epoch-local GPU words, not an SCS1/EngineDLL format. Directory is
// indexed by runtime card; records are sorted original TriangleIDs. Pixels
// are card-local, and the consumer must revalidate actual atlas ID/depth.
struct TriangleSupportIndex {
    static constexpr uint32_t Magic=0x53434831u, MaxWords=4u*1024u*1024u;
    std::vector<uint32_t> words;
    std::vector<uint32_t> complete;
    bool presence_finalized{false};
    bool owner_masks_finalized{false};
    TriangleSupportIndex(uint32_t cards,uint32_t epoch,bool presence=false):words(4ull+2ull*cards,0u),complete(presence?cards:0u,0u) {
        words[0]=Magic;words[1]=epoch;words[2]=cards;words[3]=uint32_t(words.size());
    }
    bool Add(uint32_t card,uint32_t res,std::span<const uint32_t> ids,
             std::span<const float> depths,std::span<const uint8_t> albedo) {
        const uint64_t n=uint64_t(res)*res;
        if(presence_finalized || owner_masks_finalized || card>=words[2] || res<2 || res>128 || ids.size()!=n || depths.size()!=n || albedo.size()!=n*4 || words[5+2*card] || (!complete.empty() && complete[card]))return false;
        struct Pixel {int32_t x,y;};
        auto cross=[](Pixel a,Pixel b,Pixel c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
        std::map<uint32_t,std::vector<Pixel>> groups;
        for(uint32_t x=0;x<res;++x)for(uint32_t y=0;y<res;++y) {
            const uint32_t at=y*res+x,bits=std::bit_cast<uint32_t>(depths[at]);
            if(!complete.empty() && ids[at]!=0xffffffffu)groups.try_emplace(ids[at]);
            if(ids[at]==0xffffffffu || albedo[at*4+3]<128 || (bits&0x7f800000u)==0x7f800000u || depths[at]<0)continue;
            groups[ids[at]].push_back({int32_t(x),int32_t(y)});
        }
        std::vector<uint32_t> records,vertices;
        for(const auto& [id,p]:groups) {
            if(p.size()<2){if(!complete.empty())records.insert(records.end(),{id,0u,0u});continue;}
            std::vector<Pixel> hull(2*p.size());size_t k=0;
            for(auto a:p){while(k>=2 && cross(hull[k-2],hull[k-1],a)<=0)--k;hull[k++]=a;}
            const size_t lower=k+1;
            for(int64_t i=int64_t(p.size())-2;i>=0;--i){while(k>=lower && cross(hull[k-2],hull[k-1],p[i])<=0)--k;hull[k++]=p[i];}
            if(k>1)--k;
            if(k<2 || k>64){if(!complete.empty())records.insert(records.end(),{id,0u,0u});continue;} // bounded support; never truncate a hull
            records.insert(records.end(),{id,uint32_t(vertices.size()),uint32_t(k)});
            for(size_t i=0;i<k;++i)vertices.push_back(uint32_t(hull[i].x)|(uint32_t(hull[i].y)<<16));
        }
        if(words.size()+records.size()+vertices.size()+(complete.empty()?0u:complete.size()+4u)>MaxWords)return false;
        const uint32_t offset=uint32_t(words.size()),vertexOffset=offset+uint32_t(records.size());
        for(size_t i=1;i<records.size();i+=3)records[i]+=vertexOffset;
        words[4+2*card]=offset;words[5+2*card]=uint32_t(records.size()/3);
        words.insert(words.end(),records.begin(),records.end());words.insert(words.end(),vertices.begin(),vertices.end());
        words[3]=uint32_t(words.size());if(!complete.empty())complete[card]=1u;return true;
    }
    // Reverse exact membership. Unknown cards remain permitted for ALL IDs.
    // Append before the completeness flags; legacy hull bounds stay unchanged.
    bool FinalizeOwnerMasks(std::span<const uint32_t> owners,uint32_t ownerCount) {
        if(owner_masks_finalized)return true;
        if(presence_finalized || complete.empty() || owners.size()!=complete.size() ||
           ownerCount==0 || ownerCount>MaxWords/7u)return false;
        std::vector<std::vector<uint32_t>> ownerCards(ownerCount);
        for(uint32_t ci=0;ci<owners.size();++ci){if(owners[ci]>=ownerCount)return false;ownerCards[owners[ci]].push_back(ci);}
        const uint32_t base=uint32_t(words.size());
        std::vector<uint32_t> payload(uint64_t(ownerCount)*7u,0u);
        for(uint32_t owner=0;owner<ownerCount;++owner){
            const auto& list=ownerCards[owner];if(list.empty())continue;
            const uint32_t first=list.front(),span=list.back()-first+1u;
            if(span>64u)continue; // Original per-card lookup, never truncate.
            uint64_t unknown=0;std::map<uint32_t,uint64_t> members;
            for(uint32_t ci:list){
                const uint64_t bit=uint64_t(1)<<(ci-first);
                if(!complete[ci]){unknown|=bit;continue;}
                const uint32_t at=words[4u+ci*2u],n=words[5u+ci*2u];
                if(at>words[3] || n>16384u || n>(words[3]-at)/3u)return false;
                for(uint32_t k=0;k<n;++k)members[words[at+k*3u]]|=bit;
            }
            if(members.size()>65536u)continue;
            if(uint64_t(base)+payload.size()+members.size()*3u+complete.size()+9u>MaxWords)return false;
            const uint32_t record=base+uint32_t(payload.size());
            uint32_t* row=payload.data()+owner*7u;
            row[0]=first;row[1]=span;row[2]=uint32_t(unknown);row[3]=uint32_t(unknown>>32u);
            row[4]=record;row[5]=uint32_t(members.size());row[6]=1u;
            for(const auto& [id,mask]:members)payload.insert(payload.end(),{id,uint32_t(mask),uint32_t(mask>>32u)});
        }
        if(uint64_t(base)+payload.size()+complete.size()+9u>MaxWords)return false;
        const uint32_t end=base+uint32_t(payload.size());
        words.insert(words.end(),payload.begin(),payload.end());
        words.insert(words.end(),{0x53434f31u,base,ownerCount,end,words[3]});
        owner_masks_finalized=true;return true;
    }
    // Internal optional sideband. Legacy header size still bounds hull records;
    // footer certifies cards whose sorted directory includes ALL raw IDs,
    // including singleton, invalid-depth and transparent pixels.
    bool FinalizePresence() {
        if(presence_finalized)return true;
        if(complete.empty() || words.size()+complete.size()+4u>MaxWords)return false;
        const uint32_t offset=uint32_t(words.size());
        words.insert(words.end(),complete.begin(),complete.end());
        words.insert(words.end(),{0x53435031u,offset,uint32_t(complete.size()),words[3]});
        presence_finalized=true;
        return true;
    }
};
}
