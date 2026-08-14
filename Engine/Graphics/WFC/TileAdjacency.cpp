// Engine/Graphics/WFC/TileAdjacency.cpp
#include "TileAdjacency.h"
#include "AutoSocketClassifier.h"
#include "WFCSocketOps.h"
#include "WFCTileRegistry.h"

namespace primal::graphics::wfc {

namespace {

// Build a Y-axis rotation matrix for variant N (right-hand rule, +N*90°).
// Convention matches WFCSocketOps::RotateCornerY:
//   variant 0 = identity
//   variant 1 = +90° Y: (x, z) → (z, -x)
//   variant 2 = 180°  : (x, z) → (-x, -z)
//   variant 3 = -90° Y: (x, z) → (-z, x)
//
// Columns are constructed directly because math::m4x4 lays out as
// `columns[4]` (each entry a column vector). This convention matches
// AutoSocketClassifier::TransformPoint, which reads m.columns[i][j] for
// row j of column i — the same on simd::float4x4 (Apple) and on the
// struct m4x4 fallback (WASM/other).
math::m4x4 ComputeVariantTransformY(u32 variant) {
    math::m4x4 m{};
    switch (variant & 3u) {
        case 0:  // identity
            m.columns[0] = math::v4{1, 0, 0, 0};
            m.columns[1] = math::v4{0, 1, 0, 0};
            m.columns[2] = math::v4{0, 0, 1, 0};
            m.columns[3] = math::v4{0, 0, 0, 1};
            break;
        case 1:  // +90° Y: +X → -Z, +Z → +X
            m.columns[0] = math::v4{0, 0, -1, 0};
            m.columns[1] = math::v4{0, 1,  0, 0};
            m.columns[2] = math::v4{1, 0,  0, 0};
            m.columns[3] = math::v4{0, 0,  0, 1};
            break;
        case 2:  // 180°: +X → -X, +Z → -Z
            m.columns[0] = math::v4{-1, 0,  0, 0};
            m.columns[1] = math::v4{ 0, 1,  0, 0};
            m.columns[2] = math::v4{ 0, 0, -1, 0};
            m.columns[3] = math::v4{ 0, 0,  0, 1};
            break;
        case 3:  // -90° Y: +X → +Z, +Z → -X
            m.columns[0] = math::v4{ 0, 0, 1, 0};
            m.columns[1] = math::v4{ 0, 1, 0, 0};
            m.columns[2] = math::v4{-1, 0, 0, 0};
            m.columns[3] = math::v4{ 0, 0, 0, 1};
            break;
    }
    return m;
}

} // anonymous namespace

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

bool TileAdjacencyTable::HasAnyPair(wfc_tile_id a, u32 a_var, WFCFace face) const {
    // Phase C.1 T24: prefix-scan that returns true on the first match without
    // materializing a vector. Identical mask/prefix construction to
    // GetCompatible so the two queries agree on every key.
    const u64 prefix_mask = (static_cast<u64>(0xFFFF) << 48)  // tile_a
                          | (static_cast<u64>(0xFF)   << 40)  // variant_a
                          | (static_cast<u64>(0xF)    << 36); // face
    const u64 prefix = (static_cast<u64>(static_cast<u32>(a) & 0xFFFF) << 48)
                     | (static_cast<u64>(a_var & 0xFF) << 40)
                     | (static_cast<u64>(static_cast<u32>(face) & 0xF) << 36);
    for (u64 key : compatibility_set_) {
        if ((key & prefix_mask) == (prefix & prefix_mask)) {
            return true;
        }
    }
    return false;
}

u32 TileAdjacencyTable::AddAutoFromSockets(const WFCTileRegistry& reg, bool skip_existing) {
    u32 added = 0;
    const u32 tile_count = reg.Count();
    for (u32 ta = 0; ta < tile_count; ++ta) {
        const WFCTile& tile_a = reg.Get(wfc_tile_id{ta});
        for (u32 va = 0; va < tile_a.variant_count; ++va) {
            for (u32 tb = 0; tb < tile_count; ++tb) {
                const WFCTile& tile_b = reg.Get(wfc_tile_id{tb});
                for (u32 vb = 0; vb < tile_b.variant_count; ++vb) {
                    for (u32 f = 0; f < WFC_FACE_COUNT_3D; ++f) {
                        WFCFace face_a = static_cast<WFCFace>(f);
                        WFCFace face_b = OppositeFace(face_a);
                        u8 sig_a = ComputeFaceSignature(tile_a, va, face_a);
                        u8 sig_b = ComputeFaceSignature(tile_b, vb, face_b);
                        if (!AreSocketsCompatible(sig_a, sig_b, face_a)) continue;
                        if (skip_existing &&
                            Compatible(wfc_tile_id{ta}, va, face_a,
                                       wfc_tile_id{tb}, vb)) {
                            continue;
                        }
                        AddCompatibility(wfc_tile_id{ta}, va, face_a,
                                         wfc_tile_id{tb}, vb);
                        ++added;
                    }
                }
            }
        }
    }
    return added;
}

u32 TileAdjacencyTable::BuildFromClassifier(const WFCTileRegistry& reg,
                                            ClassifierMeshLookup lookup) {
    u32 added = 0;
    if (!lookup) return 0;
    const u32 tile_count = reg.Count();
    for (u32 ta = 0; ta < tile_count; ++ta) {
        const WFCTile& tile_a = reg.Get(wfc_tile_id{ta});
        for (u32 va = 0; va < tile_a.variant_count; ++va) {
            const graphics::rhi::RHIMeshAsset* mesh_a = lookup(ta, va);
            if (!mesh_a) continue;
            const math::m4x4 var_a = ComputeVariantTransformY(va);
            for (u32 f = 0; f < WFC_FACE_COUNT_3D; ++f) {
                const WFCFace face_a = static_cast<WFCFace>(f);
                const SocketEncoding sig_a = AutoSocketClassifier::ClassifyFace(
                    *mesh_a, face_a, var_a);
                for (u32 tb = ta; tb < tile_count; ++tb) {
                    const WFCTile& tile_b = reg.Get(wfc_tile_id{tb});
                    for (u32 vb = 0; vb < tile_b.variant_count; ++vb) {
                        const graphics::rhi::RHIMeshAsset* mesh_b = lookup(tb, vb);
                        if (!mesh_b) continue;
                        const math::m4x4 var_b = ComputeVariantTransformY(vb);
                        const WFCFace face_b = OppositeFace(face_a);
                        const SocketEncoding sig_b = AutoSocketClassifier::ClassifyFace(
                            *mesh_b, face_b, var_b);
                        const SocketEncoding sig_b_mirror =
                            AutoSocketClassifier::MirrorFlipU(sig_b);
                        if (sig_a != sig_b_mirror) continue;
                        AddCompatibility(wfc_tile_id{ta}, va, face_a,
                                         wfc_tile_id{tb}, vb);
                        ++added;
                    }
                }
            }
        }
    }
    return added;
}

} // namespace primal::graphics::wfc
