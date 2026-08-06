// Engine/Graphics/WFC/ProceduralRoomPack.h
//
// Catalog of 12 procedurally-generated room tiles: 3 footprint sizes (3×3,
// 5×5, 7×7) × 4 door configurations (single door, opposite pair N-S, opposite
// pair E-W, all four). Used by WFC as a third tile source (alongside Kenney
// and Ruins) to stress-test the mixed-category solver without depending on
// external art assets.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Common/Id.h"
#include "../../Geometry/GeometryTypes.h"
#include "WFCCategory.h"

namespace primal::graphics::wfc {

class ProceduralRoomPack {
public:
    struct RoomTileDef {
        const char*  name;
        u32          footprint_cells;   // 3, 5, or 7
        u32          door_mask;         // bit 0=+X, 1=-X, 2=+Z, 3=-Z
        WFCCategory  category;          // always Primitive
    };

    static constexpr u32 kTileCount = 12;

    // Catalog accessor — returns a reference to the internal constexpr table
    // so callers can iterate without copying.
    static const RoomTileDef (&TileDefs())[kTileCount];

    // Generate mesh for tile N (0..11). Returns a geometry_id registered with
    // the engine content module. Mesh: floor + ceiling + 4 walls with door
    // openings per door_mask. Implementation lives in ProceduralRoomPack.cpp.
    static geometry::geometry_id GenerateTileMesh(u32 tile_index);
};

} // namespace primal::graphics::wfc
