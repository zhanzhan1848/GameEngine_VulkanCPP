// Engine/Graphics/WFC/WaveGrid.cpp
#include "WaveGrid.h"
#include "WFCTileRegistry.h"
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

void WaveGrid::Reset() {
    const u32 count = cells_.size();
    for (u32 i = 0; i < count; ++i) {
        cells_[i] = WFCCell{};
    }
    for (u32 i = 0; i < count; ++i) {
        propagation_dirty_[i] = 0;
    }
}

void WaveGrid::Resize(WFCGridCoord new_size) {
    assert(new_size.x > 0 && new_size.y > 0 && new_size.z > 0);
    size_ = new_size;

    const u32 count = static_cast<u32>(new_size.x) * new_size.y * new_size.z;
    cells_.resize(count);
    propagation_dirty_.resize(count, 0);

    // Reset all cells to default state — preserve no state across resize
    // (solver state is invalid for new topology anyway)
    for (u32 i = 0; i < count; ++i) {
        cells_[i] = WFCCell{};
    }
}

void WaveGrid::SetCandidateCount(WFCGridCoord c, u32 count) {
    WFCCell& cell = CellAt(c);
    for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
        cell.candidate_mask[w] = 0;
    }
    const u32 bit_cap = WFCCell::kMaskWords * 64u;
    const u32 set_count = (count < bit_cap) ? count : bit_cap;
    for (u32 b = 0; b < set_count; ++b) {
        u32 w = WFCTileRegistry::MaskWordForBit(b);
        u32 in_word = WFCTileRegistry::MaskBitInWord(b);
        cell.candidate_mask[w] |= (1ULL << in_word);
    }
    cell.candidate_count = count;
    cell.entropy = static_cast<u8>(count);
    cell.collapsed = false;
}

bool WaveGrid::HasCandidateBit(WFCGridCoord c, u32 bit) const {
    const WFCCell& cell = CellAt(c);
    if (bit >= WFCCell::kMaskWords * 64u) return false;
    u32 w = WFCTileRegistry::MaskWordForBit(bit);
    u32 b = WFCTileRegistry::MaskBitInWord(bit);
    return (cell.candidate_mask[w] & (1ULL << b)) != 0;
}

} // namespace primal::graphics::wfc
