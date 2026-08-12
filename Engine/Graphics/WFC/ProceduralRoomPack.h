// Engine/Graphics/WFC/ProceduralRoomPack.h
//
// Catalog of 12 procedurally-generated room tiles: 3 footprint sizes (3×3,
// 5×5, 7×7) × 4 door configurations (single door, opposite pair N-S, opposite
// pair E-W, all four). Used by WFC as a third tile source (alongside Kenney
// and Ruins) to stress-test the mixed-category solver without depending on
// external art assets.
//
// All tiles are emitted into a unit cube (extent ±0.5) so they fit the WFC
// wave grid (1m × 1m × 1m cells). footprint_cells is metadata only — it
// identifies the tile topology but does not affect mesh size.
#pragma once

#include "../RHI/Core/RHIMeshAsset.h"
#include "WFCCategory.h"

namespace primal::graphics::wfc {

class ProceduralRoomPack {
public:
    struct RoomTileDef {
        const char*  name;
        u32          footprint_cells;   // 3, 5, or 7 (metadata only)
        u32          door_mask;         // bit 0=+X, 1=-X, 2=+Z, 3=-Z
        WFCCategory  category;          // always Primitive
    };

    static constexpr u32 kTileCount = 12;

    // Catalog accessor — returns a reference to the internal constexpr table
    // so callers can iterate without copying.
    static const RoomTileDef (&TileDefs())[kTileCount];

    // Populate `out` with the tile's mesh (floor + ceiling + 4 walls with
    // optional door cutouts per door_mask). Caller is responsible for
    // registering the asset via content::RegisterProceduralMesh if a
    // geometry_id is needed.
    static void GenerateTileMesh(u32 tile_index, graphics::rhi::RHIMeshAsset& out);
};

} // namespace primal::graphics::wfc
