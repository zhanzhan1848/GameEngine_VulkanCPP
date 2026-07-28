#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/Nodes/ParametricTileNode.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestParametricTileNode_Execute_Emits_Catalog_PointSet() {
    ParametricTileNode node;
    node.Execute();

    TEST_ASSERT_EQ(1u, node.outputs.size(), "One output pin");
    PCGPointSet* ps = node.outputs[0].AsPointSet();
    TEST_ASSERT_NOT_NULL(ps, "Output is a PCGPointSet");
    TEST_ASSERT_EQ(5u, ps->count, "Point set has 5 entries (one per catalog tile)");
    return TestResult::Passed;
}

TestResult TestParametricTileNode_MeshIndex_Attrs_Match_Catalog() {
    ParametricTileNode node;
    node.Execute();

    PCGPointSet* ps = node.outputs[0].AsPointSet();
    TEST_ASSERT_NOT_NULL(ps, "Output is a PCGPointSet");
    for (u32 i = 0; i < ps->count; ++i) {
        f32 mesh_idx = ps->GetAttr(i, PCGAttr::MeshIndex);
        TEST_ASSERT(mesh_idx >= 1000.0f && mesh_idx <= 1004.0f,
                    "MeshIndex attr in catalog placeholder range");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ParametricTileNode");
    TEST_CASE(suite, "Execute_Emits_Catalog_PointSet",
              TestParametricTileNode_Execute_Emits_Catalog_PointSet);
    TEST_CASE(suite, "MeshIndex_Attrs_Match_Catalog",
              TestParametricTileNode_MeshIndex_Attrs_Match_Catalog);
    suite.RunAllTests();
    return 0;
}
