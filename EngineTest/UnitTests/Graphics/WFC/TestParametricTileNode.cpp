#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/Nodes/ParametricTileNode.h"
#include "Engine/Graphics/PCG/PCGTypes.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"

using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestParametricTileNode_Execute_Emits_Catalog_PointSet() {
    ParametricTileNode node;
    node.Execute();

    TEST_ASSERT_EQ(1u, node.outputs.size(), "One output pin");
    PCGPointSet* ps = node.outputs[0].AsPointSet();
    TEST_ASSERT_NOT_NULL(ps, "Output is a PCGPointSet");
    // 断言原为硬编码 5（Phase A.3 时代 Primitive tile 数）；Phase C.1 Ruins
    // 扩容到 15 后滞后（占位 mesh 2000-2900 也超出原 1000-1004 区间断言）。
    // 改为与 catalog 事实源对照：本测试自建 registry 数 Count，catalog 再
    // 扩容本断言仍正确。
    WFCTileRegistry registry;
    TileAdjacencyTable adjacency;
    WFCTileCatalog::Populate(registry, adjacency);
    TEST_ASSERT_EQ(registry.Count(), ps->count, "Point set count matches catalog tile count");
    return TestResult::Passed;
}

TestResult TestParametricTileNode_MeshIndex_Attrs_Match_Catalog() {
    ParametricTileNode node;
    node.Execute();

    PCGPointSet* ps = node.outputs[0].AsPointSet();
    TEST_ASSERT_NOT_NULL(ps, "Output is a PCGPointSet");
    WFCTileRegistry registry;
    TileAdjacencyTable adjacency;
    WFCTileCatalog::Populate(registry, adjacency);
    TEST_ASSERT_EQ(registry.Count(), ps->count, "Catalog changed between executes");
    for (u32 i = 0; i < ps->count; ++i) {
        const WFCTile& tile = registry.Get(wfc_tile_id{i});
        f32 mesh_idx = ps->GetAttr(i, PCGAttr::MeshIndex);
        // 与 ParametricTileNode::BuildCatalogPointSet 的写入语义逐点对照
        // （MeshIndex = tile.mesh_handles[0]，占位符整数精确可比较）。
        TEST_ASSERT_EQ(static_cast<f32>(static_cast<u32>(tile.mesh_handles[0])), mesh_idx,
                       "MeshIndex attr equals catalog mesh handle");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ParametricTileNode");
    TEST_CASE(suite, "Execute_Emits_Catalog_PointSet",
              TestParametricTileNode_Execute_Emits_Catalog_PointSet);
    TEST_CASE(suite, "MeshIndex_Attrs_Match_Catalog",
              TestParametricTileNode_MeshIndex_Attrs_Match_Catalog);
    const auto runStats = suite.RunAllTests();
    return runStats.failedTests == 0 ? 0 : 1;
}
