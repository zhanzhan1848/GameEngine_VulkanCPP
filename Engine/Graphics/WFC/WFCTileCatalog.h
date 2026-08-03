#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;
class TileAdjacencyTable;

// Hand-authored tile catalog for Phase A.3.
// 5 tile types: cube (1 variant), ramp (4 variants), corner_in (4), corner_out (4), pillar (1).
// Total: 14 tile-variant pairs, fits within 8x8 = 64 candidate bits.
//
// Phase A.3 uses placeholder mesh_handles (sentinel IDs 1000-1004).
// Phase A.4 will swap these for real procedural mesh registrations.
class WFCTileCatalog {
public:
    // Register all catalog tiles into the registry and populate adjacency rules.
    // Tile IDs assigned: 0=cube, 1=ramp, 2=corner_in, 3=corner_out, 4=pillar.
    static void Populate(WFCTileRegistry& registry, TileAdjacencyTable& adjacency);
};

// Phase C.1 T18+: Ruins tile factories. Used by WFCTileCatalog::Populate (T22)
// and directly by integration tests. Mesh handles are placeholders until T27
// (visual demo) wires real procedural mesh registration.
WFCTile MakeBrokenCubeTile();
WFCTile MakeMossyCubeTile();
WFCTile MakeVineCubeTile();

} // namespace primal::graphics::wfc
