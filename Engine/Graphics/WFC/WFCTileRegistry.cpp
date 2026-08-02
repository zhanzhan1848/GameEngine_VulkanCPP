#include "WFCTileRegistry.h"
#include <cassert>

namespace primal::graphics::wfc {

wfc_tile_id WFCTileRegistry::Register(const WFCTile& tile) {
    assert(tile.variant_count <= MaxVariantsPerTile &&
           "variant_count exceeds packing slot; would collide with next tile's bits");
    u32 idx = static_cast<u32>(tiles_.size());
    WFCTile copy = tile;
    copy.id = wfc_tile_id{idx};
    tiles_.push_back(copy);
    if (tile.variant_count > max_variants_) {
        max_variants_ = tile.variant_count;
    }
    return wfc_tile_id{idx};
}

const WFCTile& WFCTileRegistry::Get(wfc_tile_id id) const {
    assert(static_cast<u32>(id) < tiles_.size());
    return tiles_[static_cast<u32>(id)];
}

WFCTile& WFCTileRegistry::GetMutable(wfc_tile_id id) {
    assert(static_cast<u32>(id) < tiles_.size());
    return tiles_[static_cast<u32>(id)];
}

} // namespace primal::graphics::wfc
