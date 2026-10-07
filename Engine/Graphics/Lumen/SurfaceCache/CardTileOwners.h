#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace primal::graphics::lumen {
// Exact ownership at the trace's tile center; no card-count cap or scene scan on GPU.
// Ambiguous placement fails closed instead of choosing an arbitrary owner.
template<class Card>
bool BuildCardTileOwners(std::span<const Card> cards, uint32_t atlas, uint32_t tile,
                         std::vector<uint32_t>& owners) {
    if (!tile || !atlas || atlas % tile) return false;
    const uint32_t width = atlas / tile;
    owners.assign(size_t(width) * width, 0xFFFFFFFFu);
    for (uint32_t i=0; i<cards.size(); ++i) {
        const auto& c=cards[i];
        if (!c.resolution) continue;
        const uint64_t right=uint64_t(c.atlas_offset_x)+c.resolution;
        const uint64_t bottom=uint64_t(c.atlas_offset_y)+c.resolution;
        if (right>atlas || bottom>atlas) return false;
        for (uint32_t y=c.atlas_offset_y/tile; y<std::min<uint64_t>(width,(bottom+tile-1)/tile); ++y)
            for (uint32_t x=c.atlas_offset_x/tile; x<std::min<uint64_t>(width,(right+tile-1)/tile); ++x) {
                const uint32_t px=x*tile+tile/2, py=y*tile+tile/2;
                if (px<c.atlas_offset_x || px>=right || py<c.atlas_offset_y || py>=bottom) continue;
                auto& owner=owners[size_t(y)*width+x];
                if (owner!=0xFFFFFFFFu) return false;
                owner=i;
            }
    }
    return true;
}
}
