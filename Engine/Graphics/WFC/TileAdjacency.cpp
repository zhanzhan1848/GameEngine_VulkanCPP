// Engine/Graphics/WFC/TileAdjacency.cpp
#include "TileAdjacency.h"

namespace primal::graphics::wfc {

void TileAdjacencyTable::AddCompatibility(wfc_tile_id tile_a, u32 variant_a, WFCFace face_a,
                                          wfc_tile_id tile_b, u32 variant_b) {
    // Add forward direction
    compatibility_set_.insert(MakeKey(tile_a, variant_a, face_a, tile_b, variant_b));
    // Add reverse direction (mirror face)
    WFCFace face_b = OppositeFace(face_a);
    compatibility_set_.insert(MakeKey(tile_b, variant_b, face_b, tile_a, variant_a));
}

void TileAdjacencyTable::Clear() {
    compatibility_set_.clear();
}

bool TileAdjacencyTable::Compatible(wfc_tile_id a, u32 a_var, WFCFace face,
                                    wfc_tile_id b, u32 b_var) const {
    u64 key = MakeKey(a, a_var, face, b, b_var);
    return compatibility_set_.find(key) != compatibility_set_.end();
}

utl::vector<TileAdjacencyTable::Compatibility>
TileAdjacencyTable::GetCompatible(wfc_tile_id a, u32 a_var, WFCFace face) const {
    // Phase A.1: linear scan (replaced with lookup table in Phase A.2 if perf needs)
    utl::vector<Compatibility> result;
    // Mask only the 28-bit prefix (tile_a + variant_a + face); exclude pad bits [35:32]
    // reserved for future flags. Build expression from field widths so it stays in sync
    // with MakeKey's layout.
    const u64 prefix_mask = (static_cast<u64>(0xFFFF) << 48)  // tile_a
                          | (static_cast<u64>(0xFF)   << 40)  // variant_a
                          | (static_cast<u64>(0xF)    << 36); // face
    const u64 prefix = (static_cast<u64>(static_cast<u32>(a) & 0xFFFF) << 48)
                     | (static_cast<u64>(a_var & 0xFF) << 40)
                     | (static_cast<u64>(static_cast<u32>(face) & 0xF) << 36);
    for (u64 key : compatibility_set_) {
        if ((key & prefix_mask) == (prefix & prefix_mask)) {
            Compatibility c;
            c.tile = static_cast<wfc_tile_id>((key >> 16) & 0xFFFF);
            c.variant = static_cast<u32>(key & 0xFFFF);
            result.push_back(c);
        }
    }
    return result;
}

} // namespace primal::graphics::wfc
