// Engine/Graphics/WFC/WFCTypes.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Common/Id.h"
#include "../../Utilities/MathTypes.h"

namespace primal::graphics::wfc {

// Strong-typed IDs via DEFINE_TYPED_ID (debug strong type, release alias to u32)
DEFINE_TYPED_ID(wfc_tile_id);
DEFINE_TYPED_ID(wfc_cell_id);
DEFINE_TYPED_ID(wfc_socket_id);

// Socket encoding: 6 faces × 8 bits = 48 bits used, packed in u64
// Future-proof: leaves 16 bits for half-tile or sub-face variants
using SocketEncoding = u64;

constexpr u64 SOCKET_ENCODING_INVALID = 0xFFFFFFFFFFFFFFFFULL;

// Grid coordinate (signed to allow offset grids later)
struct WFCGridCoord {
    s32 x;
    s32 y;
    s32 z;

    bool operator==(const WFCGridCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
    bool operator!=(const WFCGridCoord& other) const { return !(*this == other); }
};

// Tile prototype (registry entry)
struct WFCTile {
    static constexpr u32 MaxVariants = 32;

    wfc_tile_id      id;
    const char*      name;                  // static-lifetime string (no engine string type)
    SocketEncoding   sockets[MaxVariants];  // per-variant socket encoding
    math::v3         bounds_extents;        // 16-byte aligned (simd::float3)
    u32              variant_count;
    bool             is_organic;
    bool             is_rotationally_symmetric;
    u8               _pad[6];               // explicit pad to 8-byte boundary
};

// Per-cell wave state (kept small for cache efficiency)
struct WFCCell {
    static constexpr u32 MaxTileCandidates = 64;

    // Bitset of currently-possible tile variants.
    // For >64 variants in future, this becomes utl::vector<u64>.
    // For Phase A foundation we cap at 64.
    u64              candidate_mask;
    u32              candidate_count;
    u8               entropy;               // popcount approx for fast compare
    bool             collapsed;
    wfc_tile_id      collapsed_tile;
    u32              collapsed_variant;
    u8               _pad[4];               // align to 8 bytes
};

// Recorded solve step (used by Phase A.2 solver, defined here for StepBuffer)
enum class WFCStepKind : u8 {
    Collapse   = 0,
    Propagate  = 1,
    Restart    = 2,
};

struct WFCStep {
    WFCStepKind      kind;
    WFCGridCoord     coord;
    wfc_tile_id      tile;       // valid when kind == Collapse
    u32              variant;    // valid when kind == Collapse
    u32              cell_count; // valid when kind == Propagate
    u32              generation; // valid when kind == Restart
};

// Face directions for 3D adjacency (6 faces of a cube)
// Order: +X, -X, +Y, -Y, +Z, -Z
enum class WFCFace : u8 {
    PosX = 0,
    NegX = 1,
    PosY = 2,
    NegY = 3,
    PosZ = 4,
    NegZ = 5,
};

constexpr u32 WFC_FACE_COUNT_3D = 6;
constexpr u32 WFC_FACE_COUNT_2D = 4;   // 2D uses +X, -X, +Y, -Y (no Z)

// Opposite face helper for adjacency lookup
constexpr WFCFace OppositeFace(WFCFace f) {
    // Pairs: PosX<->NegX, PosY<->NegY, PosZ<->NegZ
    // Even values (0,2,4) → +1; odd values → -1
    u32 v = static_cast<u32>(f);
    return static_cast<WFCFace>(v ^ 1);
}

} // namespace primal::graphics::wfc
