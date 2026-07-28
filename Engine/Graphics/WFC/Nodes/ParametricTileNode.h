#pragma once

#include "../../../Common/CommonHeaders.h"
#include "../../PCG/PCGNode.h"
#include "../../PCG/PCGTypes.h"

namespace primal::graphics::wfc {

// PCGNode that emits one point per tile in the hand-authored WFCTileCatalog.
//
// Each output point carries:
//   - position = (0, 0, 0)  (caller places via TransformNode downstream)
//   - MeshIndex attr = tile.mesh_handle (Phase A.3 placeholder IDs 1000-1004)
//   - ScaleX/Y/Z = 1.0
//   - RotationY  = 0.0
//   - TechniqueIndex = 0
//
// This is the bridge between the static WFC tile catalog and the PCG scatter
// pipeline: a downstream MeshAssignNode-like consumer reads MeshIndex to pick
// which procedural mesh to instance at each collapsed cell.
//
// Pin layout:
//   outputs[0] : PointSet — one point per catalog tile (no inputs)
class ParametricTileNode : public primal::graphics::pcg::PCGNode {
public:
    ParametricTileNode();

    const char* TypeName() const override { return "ParametricTileNode"; }
    void Execute() override;

private:
    void BuildCatalogPointSet();
};

} // namespace primal::graphics::wfc
