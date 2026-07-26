// Engine/Graphics/WFC/WaveGrid.cpp
#include "WaveGrid.h"
#include <cassert>

namespace primal::graphics::wfc {

void WaveGrid::Initialize(WFCGridCoord size, u32 max_tile_variants) {
    assert(size.x > 0 && size.y > 0 && size.z > 0);
    assert(max_tile_variants > 0 && max_tile_variants <= WFCCell::MaxTileCandidates);
    size_ = size;
    max_tile_variants_ = max_tile_variants;

    const u32 count = static_cast<u32>(size.x) * size.y * size.z;
    cells_.resize(count);
    propagation_dirty_.resize(count, 0);

    for (u32 i = 0; i < count; ++i) {
        cells_[i] = WFCCell{};
    }
}

WFCCell& WaveGrid::CellAt(WFCGridCoord c) {
    assert(c.x >= 0 && c.x < size_.x);
    assert(c.y >= 0 && c.y < size_.y);
    assert(c.z >= 0 && c.z < size_.z);
    return cells_[CoordToIndex(c)];
}

const WFCCell& WaveGrid::CellAt(WFCGridCoord c) const {
    assert(c.x >= 0 && c.x < size_.x);
    assert(c.y >= 0 && c.y < size_.y);
    assert(c.z >= 0 && c.z < size_.z);
    return cells_[CoordToIndex(c)];
}

} // namespace primal::graphics::wfc
