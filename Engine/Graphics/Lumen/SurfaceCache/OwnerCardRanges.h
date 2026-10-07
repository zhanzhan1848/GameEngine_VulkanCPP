#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
namespace primal::graphics::lumen {
// Enable bounded shader owner searches only if lookups partition ALL cards
// and every range has a single owner. Never infer ownership from an AABB.
template<class Card,class Lookup>
bool ValidateOwnerCardRanges(const std::vector<Card>& cards,const std::vector<Lookup>& lookups){
    std::size_t next=0;
    for(const auto& range:lookups){
        if(range.card_start!=next || range.card_count>cards.size()-next)return false;
        if(range.card_count){const auto owner=cards[next].mesh_instance_id;
            for(std::size_t i=next;i<next+range.card_count;++i)if(cards[i].mesh_instance_id!=owner)return false;}
        next+=range.card_count;
    }
    return next==cards.size();
}
// Internal GPU sideband: row[owner]._pad={begin,end}; its AABB/card range
// remains untouched. Reject sparse owners that cannot fit this existing table.
template<class Card,class Lookup>
bool BuildOwnerCardDirectory(const std::vector<Card>& cards,const std::vector<Lookup>& lookups,std::vector<Lookup>& output){
    if(cards.size()>std::numeric_limits<uint32_t>::max() || !ValidateOwnerCardRanges(cards,lookups)){
        output.clear();return false;
    }
    auto built=lookups;
    for(auto& row:built)row._pad[0]=row._pad[1]=0;
    for(const auto& range:lookups){
        if(!range.card_count)continue;
        const auto owner=cards[range.card_start].mesh_instance_id;
        if(owner>=built.size()){output.clear();return false;}
        auto& row=built[owner];
        row._pad[0]=row._pad[1]?std::min(row._pad[0],range.card_start):range.card_start;
        row._pad[1]=std::max(row._pad[1],range.card_start+range.card_count);
    }
    output=std::move(built);return true;
}
}
