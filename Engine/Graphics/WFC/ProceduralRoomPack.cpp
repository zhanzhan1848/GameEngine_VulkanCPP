// Engine/Graphics/WFC/ProceduralRoomPack.cpp
#include "ProceduralRoomPack.h"

#include <cassert>

namespace primal::graphics::wfc {

namespace {

// 12 tiles: 3 footprint sizes × 4 door configurations.
//
// door_mask bit layout (matches test convention in TestProceduralRoomPack):
//   bit 0 (+X)  bit 1 (-X)  bit 2 (+Z)  bit 3 (-Z)
//
// The four door configurations used here:
//   0x1 = +X only            (single door)
//   0x3 = +X + -X            (opposite pair, X axis)
//   0xC = +Z + -Z            (opposite pair, Z axis)
//   0xF = all four doors     (4-way intersection)
constexpr ProceduralRoomPack::RoomTileDef kTiles[ProceduralRoomPack::kTileCount] = {
    // 3×3 footprint
    { "proc_room_3x3_door_n",  3, 0x1, WFCCategory::Primitive },
    { "proc_room_3x3_door_ns", 3, 0x3, WFCCategory::Primitive },
    { "proc_room_3x3_door_ew", 3, 0xC, WFCCategory::Primitive },
    { "proc_room_3x3_door_4",  3, 0xF, WFCCategory::Primitive },
    // 5×5 footprint
    { "proc_room_5x5_door_n",  5, 0x1, WFCCategory::Primitive },
    { "proc_room_5x5_door_ns", 5, 0x3, WFCCategory::Primitive },
    { "proc_room_5x5_door_ew", 5, 0xC, WFCCategory::Primitive },
    { "proc_room_5x5_door_4",  5, 0xF, WFCCategory::Primitive },
    // 7×7 footprint
    { "proc_room_7x7_door_n",  7, 0x1, WFCCategory::Primitive },
    { "proc_room_7x7_door_ns", 7, 0x3, WFCCategory::Primitive },
    { "proc_room_7x7_door_ew", 7, 0xC, WFCCategory::Primitive },
    { "proc_room_7x7_door_4",  7, 0xF, WFCCategory::Primitive },
};

} // namespace

const ProceduralRoomPack::RoomTileDef (&ProceduralRoomPack::TileDefs())[kTileCount] {
    return kTiles;
}

// Stub — real generator (floor + ceiling + walls + door cutouts) lands in the
// next task. Returns invalid_id so callers fail loudly if invoked early.
geometry::geometry_id ProceduralRoomPack::GenerateTileMesh(u32 tile_index) {
    assert(tile_index < kTileCount);
    (void)tile_index;
    return geometry::geometry_id{ id::invalid_id };
}

} // namespace primal::graphics::wfc
