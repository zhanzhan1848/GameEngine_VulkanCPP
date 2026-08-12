// Engine/Graphics/WFC/WaveGrid.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WaveGrid {
public:
    void Initialize(WFCGridCoord size, u32 max_tile_variants);
    void Resize(WFCGridCoord new_size);   // Task 4
    void Reset();                          // Task 5

    WFCCell&       CellAt(WFCGridCoord c);
    const WFCCell& CellAt(WFCGridCoord c) const;

    // Test/setup convenience: set the first `count` candidate bits (bits 0..count-1)
    // and clear the rest. Useful for tests that want "this cell has N candidates
    // possible" without constructing a specific mask across the u64[kMaskWords]
    // bitset. Bits ≥ kMaskWords*64 are ignored.
    void SetCandidateCount(WFCGridCoord c, u32 count);

    // Test/introspection helper: true iff candidate bit `bit` is set on cell c.
    // `bit` indexes the same packing used by WFCTileRegistry::BitForTileVariant.
    bool HasCandidateBit(WFCGridCoord c, u32 bit) const;

    WFCGridCoord   Size() const { return size_; }
    u32            CellCount() const { return cells_.size(); }
    u32            BytesPerCell() const { return sizeof(WFCCell); }
    u32            MaxTileVariants() const { return max_tile_variants_; }

    // For range-based iteration
    utl::vector<WFCCell>&       CellsMutable() { return cells_; }
    const utl::vector<WFCCell>& Cells() const { return cells_; }

private:
    WFCGridCoord         size_{};
    u32                  max_tile_variants_{0};
    utl::vector<WFCCell> cells_;
    utl::vector<u8>      propagation_dirty_;  // Task 8 (Propagator)

    u32   CoordToIndex(WFCGridCoord c) const {
        return static_cast<u32>(c.x) +
               static_cast<u32>(c.y) * size_.x +
               static_cast<u32>(c.z) * size_.x * size_.y;
    }
};

} // namespace primal::graphics::wfc
