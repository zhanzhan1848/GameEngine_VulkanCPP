// Engine/Graphics/WFC/TileAdjacency.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"
#include <unordered_set>

namespace primal::graphics::wfc {

class WFCTileRegistry;  // forward decl — full definition in WFCTileRegistry.h

// TileAdjacencyTable — records which (tile, variant) pairs may be placed next
// to each other along a given face. Stores compatibilities as a hash set of
// packed 64-bit keys for O(1) lookup. AddCompatibility automatically records
// the mirror entry so callers only declare one side of a symmetric pair.
class TileAdjacencyTable {
public:
    struct Compatibility {
        wfc_tile_id  tile;
        u32          variant;
    };

    // Records: "(tile_a, variant_a) at face_a accepts (tile_b, variant_b) on the opposite face."
    // Also records the mirror automatically (face_b = OppositeFace(face_a)).
    void AddCompatibility(wfc_tile_id tile_a, u32 variant_a, WFCFace face_a,
                          wfc_tile_id tile_b, u32 variant_b);

    void Clear();

    // Returns true if (a, a_var) at face accepts (b, b_var) on opposite face.
    bool Compatible(wfc_tile_id a, u32 a_var, WFCFace face,
                    wfc_tile_id b, u32 b_var) const;

    // Phase A.2 lookup: returns all (tile, variant) pairs compatible with (a, a_var) at face.
    utl::vector<Compatibility> GetCompatible(wfc_tile_id a, u32 a_var, WFCFace face) const;

    // Phase C.1 Task 16: iterate all (tile_a, variant_a, face_a) x
    // (tile_b, variant_b, face_b=opposite) pairs in `reg`, and call
    // AddCompatibility for each pair whose per-face socket signatures are
    // compatible per AreSocketsCompatible (strict OR mirror match).
    //
    // skip_existing=true: when Compatible(a, va, fa, b, vb) already returns
    // true (e.g. because the mirror entry was recorded by a prior symmetric
    // iteration), skip the AddCompatibility call so the entry isn't double-
    // counted in the return value. AddCompatibility's auto-mirror behaviour
    // already prevents duplicate set entries; this flag only affects the
    // returned count.
    //
    // Returns the count of newly-added forward entries (one per
    // AddCompatibility call that actually fired).
    u32 AddAutoFromSockets(const WFCTileRegistry& reg, bool skip_existing = true);

private:
    // Pack (tile_a, variant_a, face, tile_b, variant_b) into u64 key
    // Layout: [tile_a:16][variant_a:8][face:4][pad:4][tile_b:16][variant_b:16] = 64 bits
    static u64 MakeKey(wfc_tile_id tile_a, u32 variant_a, WFCFace face,
                       wfc_tile_id tile_b, u32 variant_b) {
        u64 k = 0;
        k |= (static_cast<u64>(static_cast<u32>(tile_a) & 0xFFFF) << 48);
        k |= (static_cast<u64>(variant_a & 0xFF) << 40);
        k |= (static_cast<u64>(static_cast<u32>(face) & 0xF) << 36);
        // bits 32-35 are pad/reserved
        k |= (static_cast<u64>(static_cast<u32>(tile_b) & 0xFFFF) << 16);
        k |= (static_cast<u64>(variant_b & 0xFFFF) << 0);
        return k;
    }

    std::unordered_set<u64> compatibility_set_;
};

} // namespace primal::graphics::wfc
