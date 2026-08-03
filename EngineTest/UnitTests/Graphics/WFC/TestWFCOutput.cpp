#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

#include <cmath>

using namespace primal::geometry;
using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestWFCOutput_Empty_Step_Buffer_Produces_Empty_Point_Set() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.mesh_handles[0] = geometry_id{42};
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
    t.mesh_handles[0] = geometry_id{42};
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
    t.mesh_handles[0] = geometry_id{777};
    reg.Register(t);

    WFCStep step{};
    step.kind = WFCStepKind::Collapse;
    step.coord = {0, 0, 0};
    step.tile = wfc_tile_id{0};
    step.variant = 0;
    buf.Push(step);

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    f32 mesh_idx = ps.GetAttr(0, PCGAttr::MeshIndex);
    TEST_ASSERT_EQ(777.0f, mesh_idx, "MeshIndex attr = tile.mesh_handles[0]");
    return TestResult::Passed;
}

TestResult TestWFCOutput_Multi_Step_Drains_Buffer() {
    WFCStepBuffer buf;
    WFCTileRegistry reg;
    WFCTile t{};
    t.mesh_handles[0] = geometry_id{1};
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

TestResult TestWFCOutput_FlagDriven_Rotation_For_NonSymmetric_Tile() {
    // Phase B.1: WFCOutput uses WFCTile::is_rotationally_symmetric to decide
    // RotationY. Ramp/corner_in/corner_out (non-symmetric, 4 variants) get
    // variant * pi/2. Cube/pillar (symmetric) stay at 0 regardless of variant.
    WFCStepBuffer buf;
    WFCTileRegistry reg;

    // Register a non-symmetric tile with 4 variants (id=0).
    WFCTile non_symmetric{};
    non_symmetric.variant_count = 4;
    non_symmetric.is_rotationally_symmetric = false;
    reg.Register(non_symmetric);

    // Register a symmetric tile (id=1) — single variant.
    WFCTile symmetric{};
    symmetric.variant_count = 1;
    symmetric.is_rotationally_symmetric = true;
    reg.Register(symmetric);

    // Push a Collapse for non-symmetric variant 2 → expect rot_y = pi.
    WFCStep s1{};
    s1.kind = WFCStepKind::Collapse;
    s1.coord = {0, 0, 0};
    s1.tile = wfc_tile_id{0};
    s1.variant = 2;
    buf.Push(s1);

    // Push a Collapse for symmetric variant 0 → expect rot_y = 0.
    WFCStep s2{};
    s2.kind = WFCStepKind::Collapse;
    s2.coord = {1, 0, 0};
    s2.tile = wfc_tile_id{1};
    s2.variant = 0;
    buf.Push(s2);

    PCGPointSet ps = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    TEST_ASSERT_EQ(2u, ps.count, "Two collapse steps -> two points");

    constexpr f32 kHalfPi = 1.5707963267948966f;
    f32 rot0 = ps.GetAttr(0, PCGAttr::RotationY);
    f32 rot1 = ps.GetAttr(1, PCGAttr::RotationY);
    // First-emitted point is the non-symmetric variant 2 -> rot = pi.
    TEST_ASSERT(std::abs(rot0 - 2.0f * kHalfPi) < 1e-5f,
                "Non-symmetric variant 2 -> rot_y = pi");
    // Second point is symmetric -> rot = 0.
    TEST_ASSERT(std::abs(rot1) < 1e-5f,
                "Symmetric tile -> rot_y = 0");
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
    TEST_CASE(suite, "FlagDriven_Rotation_For_NonSymmetric_Tile",
              TestWFCOutput_FlagDriven_Rotation_For_NonSymmetric_Tile);
    suite.RunAllTests();
    return 0;
}
