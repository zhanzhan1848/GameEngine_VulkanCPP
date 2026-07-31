#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry {
public:
    // Registers a tile and returns its handle. The registry owns a copy.
    // id is auto-assigned (sequential starting at 0).
    wfc_tile_id Register(const WFCTile& tile);

    const WFCTile& Get(wfc_tile_id id) const;
    WFCTile&       GetMutable(wfc_tile_id id);

    u32 Count() const { return static_cast<u32>(tiles_.size()); }
    u32 MaxVariants() const { return max_variants_; }

    // Phase C.1 packing: 16 tiles × 4 variants = 64 candidates in a u64 mask.
    static constexpr u32 MaxVariantsPerTile = 4;
    static constexpr u32 MaxTiles           = 16;

    static u32 BitForTileVariant(wfc_tile_id tile, u32 variant) {
        return static_cast<u32>(tile) * MaxVariantsPerTile + variant;
    }
    static wfc_tile_id TileForBit(u32 bit) {
        return wfc_tile_id{bit / MaxVariantsPerTile};
    }
    static u32 VariantForBit(u32 bit) {
        return bit % MaxVariantsPerTile;
    }

private:
    utl::vector<WFCTile> tiles_;
    u32                  max_variants_{0};
};

} // namespace primal::graphics::wfc
