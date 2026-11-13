// EngineTest/UnitTests/Graphics/WFC/TestTileAdjacencyBuild.cpp
//
// Phase C.1 Task 11: TileAdjacencyTable::BuildFromClassifier.
// Verifies the AutoSocketClassifier-based adjacency builder records
// compatibility entries when two tiles' 8×8 occupancy grids agree
// (after MirrorFlipU on the opposing face).
#include "../../TestFramework.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

#include <vector>

using namespace primal::graphics::wfc;
using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace primal::content;
using namespace Engine::Test;

namespace {

// Test fixture: two solid cubes (emit_box_geometry) wired to tiles 0 and 1.
// The lookup callback returns mesh-by-tile-index so BuildFromClassifier can
// reach the RHIMeshAsset without going through geometry_id.
struct TwoCubeSetup {
    RHIMeshAsset mesh_a;
    RHIMeshAsset mesh_b;
    WFCTileRegistry reg;
    std::vector<const RHIMeshAsset*> meshes;

    TwoCubeSetup() {
        emit_box_geometry(mesh_a, 1.0f, 1.0f, 1.0f);
        emit_box_geometry(mesh_b, 1.0f, 1.0f, 1.0f);

        WFCTile ta{}; ta.name = "cube_a"; ta.variant_count = 1;
        WFCTile tb{}; tb.name = "cube_b"; tb.variant_count = 1;
        reg.Register(ta);
        reg.Register(tb);

        meshes.push_back(&mesh_a);
        meshes.push_back(&mesh_b);
    }

    const RHIMeshAsset* Lookup(u32 tile_index, u32 /*variant*/) const {
        if (tile_index >= meshes.size()) return nullptr;
        return meshes[tile_index];
    }
};

} // namespace

// Two identical cubes: every face is fully solid (sig = all ones). After
// MirrorFlipU the opposing face is still all ones, so every (face_a, face_b)
// pair is compatible. The table should record at least one entry per face.
TestResult TestBuildFromClassifierProducesEntries() {
    TwoCubeSetup s;
    TileAdjacencyTable table;
    const u32 added = table.BuildFromClassifier(
        s.reg, [&](u32 t, u32 v) -> const RHIMeshAsset* { return s.Lookup(t, v); });
    TEST_ASSERT(added > 0u, "should produce adjacencies for two identical cubes");
    // Entry count should be > 0 (mirror entries auto-added).
    TEST_ASSERT(table.EntryCount() > 0u, "table non-empty");
    return TestResult::Passed;
}

// Two identical cubes must be mutually compatible on every face pair —
// cube_a +X vs cube_b -X is the canonical +X/-X seam.
TestResult TestBuildFromClassifierCompatibleIdenticalCubes() {
    TwoCubeSetup s;
    TileAdjacencyTable table;
    table.BuildFromClassifier(
        s.reg, [&](u32 t, u32 v) -> const RHIMeshAsset* { return s.Lookup(t, v); });

    const bool compat_posX = table.Compatible(
        wfc_tile_id{0}, 0, WFCFace::PosX, wfc_tile_id{1}, 0);
    TEST_ASSERT(compat_posX, "cube_a +X compatible with cube_b -X");
    const bool compat_negX = table.Compatible(
        wfc_tile_id{1}, 0, WFCFace::NegX, wfc_tile_id{0}, 0);
    TEST_ASSERT(compat_negX, "mirror entry present");
    return TestResult::Passed;
}

// Self-adjacency: a cube must be compatible with itself on every face
// (the AddCompatibility auto-mirror behaviour + identical signatures).
TestResult TestBuildFromClassifierSelfAdjacent() {
    TwoCubeSetup s;
    TileAdjacencyTable table;
    table.BuildFromClassifier(
        s.reg, [&](u32 t, u32 v) -> const RHIMeshAsset* { return s.Lookup(t, v); });

    const bool self_posX = table.Compatible(
        wfc_tile_id{0}, 0, WFCFace::PosX, wfc_tile_id{0}, 0);
    TEST_ASSERT(self_posX, "cube self-compatible on +X");
    return TestResult::Passed;
}

// Returning nullptr from the lookup should skip that pair gracefully —
// no crash, no entry added for the missing tile.
TestResult TestBuildFromClassifierHandlesNullLookup() {
    TwoCubeSetup s;
    TileAdjacencyTable table;
    const u32 added = table.BuildFromClassifier(
        s.reg, [](u32, u32) -> const RHIMeshAsset* { return nullptr; });
    TEST_ASSERT_EQ(0u, added, "no entries when lookup always returns nullptr");
    TEST_ASSERT_EQ(0u, table.EntryCount(), "table empty");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("TileAdjacencyBuild");
    TEST_CASE(suite, "BuildFromClassifierProducesEntries",
              TestBuildFromClassifierProducesEntries);
    TEST_CASE(suite, "BuildFromClassifierCompatibleIdenticalCubes",
              TestBuildFromClassifierCompatibleIdenticalCubes);
    TEST_CASE(suite, "BuildFromClassifierSelfAdjacent",
              TestBuildFromClassifierSelfAdjacent);
    TEST_CASE(suite, "BuildFromClassifierHandlesNullLookup",
              TestBuildFromClassifierHandlesNullLookup);
    suite.RunAllTests();
    return 0;
}
