#include "ParametricTileNode.h"
#include "../WFCTileCatalog.h"
#include "../WFCTileRegistry.h"
#include "../TileAdjacency.h"
#include "../WFCTypes.h"

namespace primal::graphics::wfc {

using primal::graphics::pcg::PCGAttr;
using primal::graphics::pcg::PCGDataType;
using primal::graphics::pcg::PCGPointSet;

ParametricTileNode::ParametricTileNode() {
    outputs.resize(1);
    outputs[0].expected_type = PCGDataType::PointSet;
}

void ParametricTileNode::Execute() {
    BuildCatalogPointSet();
}

void ParametricTileNode::BuildCatalogPointSet() {
    WFCTileRegistry registry;
    TileAdjacencyTable adjacency;
    WFCTileCatalog::Populate(registry, adjacency);

    const u32 tile_count = registry.Count();
    PCGPointSet* ps = CreateOutput<PCGPointSet>(0);
    ps->Init(tile_count, static_cast<u32>(PCGAttr::Count));

    for (u32 i = 0; i < tile_count; ++i) {
        const WFCTile& tile = registry.Get(wfc_tile_id{i});
        ps->positions[i] = math::v3{0.0f, 0.0f, 0.0f};
        ps->SetAttr(i, PCGAttr::ScaleX, 1.0f);
        ps->SetAttr(i, PCGAttr::ScaleY, 1.0f);
        ps->SetAttr(i, PCGAttr::ScaleZ, 1.0f);
        ps->SetAttr(i, PCGAttr::RotationY, 0.0f);
        ps->SetAttr(i, PCGAttr::MeshIndex,
                    static_cast<f32>(static_cast<u32>(tile.mesh_handle)));
        ps->SetAttr(i, PCGAttr::TechniqueIndex, 0.0f);
    }
}

} // namespace primal::graphics::wfc
