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

// Registry fixture: one tile, id 0, mesh_handles[0]=42, rotationally symmetric.
static WFCTileRegistry MakeOneTileRegistry() {
    WFCTileRegistry reg;
    WFCTile t{};
    t.name = "test";
    t.mesh_handles[0] = geometry_id{42};
    t.variant_count = 1;
    t.is_rotationally_symmetric = true;
    reg.Register(t);
    return reg;
}

static WFCStep MakeCollapse(s32 x, s32 y, s32 z, wfc_tile_id tile, u32 variant) {
    WFCStep s{};
    s.kind = WFCStepKind::Collapse;
    s.coord = {x, y, z};
    s.tile = tile;
    s.variant = variant;
    return s;
}

static WFCStep MakeRestart(u32 generation) {
    WFCStep s{};
    s.kind = WFCStepKind::Restart;
    s.generation = generation;
    return s;
}

TestResult DrainStream_EmptyBuffer_ReturnsEmpty() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 1.0f);

    TEST_ASSERT_EQ(0u, r.new_points.count, "empty buffer -> 0 points");
    TEST_ASSERT(!r.restart_seen, "empty buffer -> no restart");
    TEST_ASSERT_EQ(0u, r.restart_count, "empty buffer -> restart_count=0");
    return TestResult::Passed;
}

TestResult DrainStream_SingleCollapse_ProducesOnePoint() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();
    buf.Push(MakeCollapse(1, 2, 3, wfc_tile_id{0}, 0));

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 1.0f);

    TEST_ASSERT_EQ(1u, r.new_points.count, "1 collapse -> 1 point");
    TEST_ASSERT_EQ(1.0f, r.new_points.positions[0].x, "x = coord.x * cell_size");
    TEST_ASSERT_EQ(2.0f, r.new_points.positions[0].y, "y = coord.y * cell_size");
    TEST_ASSERT_EQ(3.0f, r.new_points.positions[0].z, "z = coord.z * cell_size");
    TEST_ASSERT(!r.restart_seen, "no restart in snapshot");
    return TestResult::Passed;
}

TestResult DrainStream_MultipleCollapses_ProducesAllPointsInOrder() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();
    buf.Push(MakeCollapse(0, 0, 0, wfc_tile_id{0}, 0));
    buf.Push(MakeCollapse(1, 0, 0, wfc_tile_id{0}, 0));
    buf.Push(MakeCollapse(2, 0, 0, wfc_tile_id{0}, 0));

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 2.0f);

    TEST_ASSERT_EQ(3u, r.new_points.count, "3 collapses -> 3 points");
    TEST_ASSERT_EQ(0.0f, r.new_points.positions[0].x, "point 0 x");
    TEST_ASSERT_EQ(2.0f, r.new_points.positions[1].x, "point 1 x (cell_size=2)");
    TEST_ASSERT_EQ(4.0f, r.new_points.positions[2].x, "point 2 x (cell_size=2)");
    return TestResult::Passed;
}

TestResult DrainStream_RestartClearsNewPoints_AndSetsFlag() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();
    // Pre-restart collapses — must be discarded.
    buf.Push(MakeCollapse(5, 5, 5, wfc_tile_id{0}, 0));
    buf.Push(MakeRestart(1));
    // Post-restart collapse — survives.
    buf.Push(MakeCollapse(1, 1, 1, wfc_tile_id{0}, 0));

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 1.0f);

    TEST_ASSERT(r.restart_seen, "restart_seen must be true");
    TEST_ASSERT_EQ(1u, r.restart_count, "one restart step");
    TEST_ASSERT_EQ(1u, r.new_points.count, "only post-restart collapse survives");
    TEST_ASSERT_EQ(1.0f, r.new_points.positions[0].x, "survivor is post-restart");
    return TestResult::Passed;
}

TestResult DrainStream_MultipleRestarts_IncrementsCountAndKeepsLastBatch() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();
    buf.Push(MakeCollapse(1, 1, 1, wfc_tile_id{0}, 0));
    buf.Push(MakeRestart(1));
    buf.Push(MakeCollapse(2, 2, 2, wfc_tile_id{0}, 0));
    buf.Push(MakeRestart(2));
    buf.Push(MakeCollapse(3, 3, 3, wfc_tile_id{0}, 0));
    buf.Push(MakeCollapse(4, 4, 4, wfc_tile_id{0}, 0));

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 1.0f);

    TEST_ASSERT(r.restart_seen, "restart_seen");
    TEST_ASSERT_EQ(2u, r.restart_count, "two restarts");
    TEST_ASSERT_EQ(2u, r.new_points.count, "post-last-restart batch has 2 collapses");
    TEST_ASSERT_EQ(3.0f, r.new_points.positions[0].x, "first survivor x=3");
    TEST_ASSERT_EQ(4.0f, r.new_points.positions[1].x, "second survivor x=4");
    return TestResult::Passed;
}

TestResult DrainStream_RestartAsLastStep_YieldsZeroPoints() {
    WFCStepBuffer buf;
    WFCTileRegistry reg = MakeOneTileRegistry();
    buf.Push(MakeCollapse(1, 1, 1, wfc_tile_id{0}, 0));
    buf.Push(MakeRestart(1));

    WFCStreamDrainResult r = WFCOutput::DrainStream(buf, reg, 1.0f);

    TEST_ASSERT(r.restart_seen, "restart_seen");
    TEST_ASSERT_EQ(1u, r.restart_count, "one restart");
    TEST_ASSERT_EQ(0u, r.new_points.count, "no post-restart collapses -> 0 points");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCStreamDrain");
    TEST_CASE(suite, "EmptyBuffer_ReturnsEmpty",
              DrainStream_EmptyBuffer_ReturnsEmpty);
    TEST_CASE(suite, "SingleCollapse_ProducesOnePoint",
              DrainStream_SingleCollapse_ProducesOnePoint);
    TEST_CASE(suite, "MultipleCollapses_ProducesAllPointsInOrder",
              DrainStream_MultipleCollapses_ProducesAllPointsInOrder);
    TEST_CASE(suite, "RestartClearsNewPoints_AndSetsFlag",
              DrainStream_RestartClearsNewPoints_AndSetsFlag);
    TEST_CASE(suite, "MultipleRestarts_IncrementsCountAndKeepsLastBatch",
              DrainStream_MultipleRestarts_IncrementsCountAndKeepsLastBatch);
    TEST_CASE(suite, "RestartAsLastStep_YieldsZeroPoints",
              DrainStream_RestartAsLastStep_YieldsZeroPoints);
    suite.RunAllTests();
    return 0;
}
