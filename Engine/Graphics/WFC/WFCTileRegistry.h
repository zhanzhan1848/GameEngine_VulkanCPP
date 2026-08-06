#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"
#include <vector>

namespace primal::graphics::wfc {

class WFCTileRegistry {
public:
    // Registers a tile and returns its handle. The registry owns a copy.
    // id is auto-assigned (sequential starting at 0).
    wfc_tile_id Register(const WFCTile& tile);

    // Phase C.1 Mixed: append every tile from a catalog vector, overwriting
    // each tile's .category with `category`. Multi-source composition uses
    // this to register Kenney (Dungeon) + Ruins + ProceduralRoomPack tiles
    // into one registry, with the category filter driving which set the
    // solver draws from per cell.
    void RegisterFromCatalog(const std::vector<WFCTile>& tiles, WFCCategory category);

    const WFCTile& Get(wfc_tile_id id) const;
    WFCTile&       GetMutable(wfc_tile_id id);

    u32 Count() const { return static_cast<u32>(tiles_.size()); }
    u32 MaxVariants() const { return max_variants_; }

    // Phase C.1 Mixed packing: 64 tiles × 4 variants = 256 candidates, spread
    // across WFCCell::candidate_mask[kMaskWords] (u64 × 4 = 256 bits).
    // As of Phase C.1 Task 4, WFCTile::MaxVariants (WFCTypes.h) is reconciled to 4,
    // matching MaxVariantsPerTile. These two constants must stay in sync.
    static constexpr u32 MaxVariantsPerTile = WFCTile::MaxVariants;
    static constexpr u32 MaxTiles           = 64;
    static constexpr u32 kBitsPerMaskWord   = 64;

    static u32 BitForTileVariant(wfc_tile_id tile, u32 variant) {
        return static_cast<u32>(tile) * MaxVariantsPerTile + variant;
    }
    static wfc_tile_id TileForBit(u32 bit) {
        return wfc_tile_id{bit / MaxVariantsPerTile};
    }
    static u32 VariantForBit(u32 bit) {
        return bit % MaxVariantsPerTile;
    }

    // Word/bit indexing into WFCCell::candidate_mask[kMaskWords].
    // Global `bit` ∈ [0, MaxTiles * MaxVariantsPerTile) maps to word (bit/kBitsPerMaskWord)
    // and in-word bit (bit%kBitsPerMaskWord). Helpers are header-only so they inline.
    static constexpr u32 MaskWordForBit(u32 bit) { return bit / kBitsPerMaskWord; }
    static constexpr u32 MaskBitInWord(u32 bit)  { return bit % kBitsPerMaskWord; }

private:
    utl::vector<WFCTile> tiles_;
    u32                  max_variants_{0};
};

} // namespace primal::graphics::wfc
