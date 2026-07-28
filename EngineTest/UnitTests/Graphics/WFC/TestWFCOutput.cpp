#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

using namespace primal::geometry;
using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestWFCOutput_Empty_Step_Buffer_Produces_Empty_Point_Set() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.mesh_handle = geometry_id{42};
    reg.Register(t);

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    TEST_ASSERT_EQ(0u, ps.count, "Empty step buffer -> empty point set");
    return TestResult::Passed;
}

TestResult TestWFCOutput_One_Collapse_Step_Produces_One_Point() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.name = "test";
    t.mesh_handle = geometry_id{42};
    reg.Register(t);

    WFCStep step{};
    step.kind = WFCStepKind::Collapse;
    step.coord = {1, 2, 3};
    step.tile = wfc_tile_id{0};
    step.variant = 0;
    buf.Push(step);

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    TEST_ASSERT_EQ(1u, ps.count, "One Collapse step -> one point");
    TEST_ASSERT_EQ(1.0f, ps.positions[0].x, "Position X = coord.x * cell_size");
    TEST_ASSERT_EQ(2.0f, ps.positions[0].y, "Position Y = coord.y * cell_size");
    TEST_ASSERT_EQ(3.0f, ps.positions[0].z, "Position Z = coord.z * cell_size");
    return TestResult::Passed;
}

TestResult TestWFCOutput_MeshIndex_Matches_Tile_Registry() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.mesh_handle = geometry_id{777};
    reg.Register(t);

    WFCStep step{};
    step.kind = WFCStepKind::Collapse;
    step.coord = {0, 0, 0};
    step.tile = wfc_tile_id{0};
    step.variant = 0;
    buf.Push(step);

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    f32 mesh_idx = ps.GetAttr(0, PCGAttr::MeshIndex);
    TEST_ASSERT_EQ(777.0f, mesh_idx, "MeshIndex attr = tile.mesh_handle");
    return TestResult::Passed;
}

TestResult TestWFCOutput_Multi_Step_Drains_Buffer() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.mesh_handle = geometry_id{1};
    reg.Register(t);

    for (u32 i = 0; i < 5; ++i) {
        WFCStep s{};
        s.kind = WFCStepKind::Collapse;
        s.coord = {static_cast<s32>(i), 0, 0};
        s.tile = wfc_tile_id{0};
        s.variant = 0;
        buf.Push(s);
    }

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    TEST_ASSERT_EQ(5u, ps.count, "5 Collapse steps -> 5 points");
    TEST_ASSERT(buf.Empty(), "Step buffer drained");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCOutput");
    TEST_CASE(suite, "Empty_Step_Buffer",
              TestWFCOutput_Empty_Step_Buffer_Produces_Empty_Point_Set);
    TEST_CASE(suite, "One_Collapse_Step",
              TestWFCOutput_One_Collapse_Step_Produces_One_Point);
    TEST_CASE(suite, "MeshIndex_Matches_Registry",
              TestWFCOutput_MeshIndex_Matches_Tile_Registry);
    TEST_CASE(suite, "Multi_Step_Drains_Buffer",
              TestWFCOutput_Multi_Step_Drains_Buffer);
    suite.RunAllTests();
    return 0;
}
