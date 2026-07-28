#pragma once

#include "../../Common/CommonHeaders.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;
class TileAdjacencyTable;

// Hand-authored tile catalog for Phase A.3.
// 5 tile types: cube (1 variant), ramp (4 variants), corner_in (1), corner_out (1), pillar (1).
// Total: 8 tile-variant pairs, fits within 8x8 = 64 candidate bits.
//
// Phase A.3 uses placeholder mesh_handles (sentinel IDs 0-4).
// Phase A.4 will swap these for real procedural mesh registrations.
class WFCTileCatalog {
public:
    // Register all catalog tiles into the registry and populate adjacency rules.
    // Tile IDs assigned: 0=cube, 1=ramp, 2=corner_in, 3=corner_out, 4=pillar.
    static void Populate(WFCTileRegistry& registry, TileAdjacencyTable& adjacency);
};

} // namespace primal::graphics::wfc
