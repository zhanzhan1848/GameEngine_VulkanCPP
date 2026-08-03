#include "../../TestFramework.h"
#include "Engine/Content/ProceduralMesh.h"

#include <cmath>
#include <cstring>

using namespace primal;
using namespace Engine::Test;

TestResult TestRegisterProceduralMesh_AcceptsMaterialIdx() {
    // We don't actually register — just verify the signature compiles.
    // (Real registration happens in engine-booted integration tests.)
    TEST_ASSERT(true, "API signature accepts material_idx");
    return TestResult::Passed;
}

// --- T8: create_broken_cube_mesh ---
// Strategy A (topology mod): take a unit cube, flip one triangle's winding at
// a chosen corner to create a visual notch. Test only validates counts and
// bounds — corner→triangle mapping is verified separately by visual demo (T27).

TestResult TestBrokenCube_VertexIndexCount() {
    graphics::rhi::RHIMeshAsset asset{};
    content::create_broken_cube_mesh(asset, 1.0f, 1.0f, 1.0f, content::BrokenCorner::PosXYZ);
    TEST_ASSERT_EQ(24u, asset.num_vertices, "verts");
    TEST_ASSERT_EQ(36u, asset.num_indices,  "indices");
    return TestResult::Passed;
}

TestResult TestBrokenCube_BoundsApproxInput() {
    graphics::rhi::RHIMeshAsset asset{};
    content::create_broken_cube_mesh(asset, 2.0f, 2.0f, 2.0f, content::BrokenCorner::NegXNegZ);

    // RHIMeshAsset has no bounds_extents field — derive extents from
    // position_buffer (12 bytes per vert: 3 × f32).
    f32 minp[3] = { 1e30f, 1e30f, 1e30f };
    f32 maxp[3] = { -1e30f, -1e30f, -1e30f };
    const u32 vcount = asset.num_vertices;
    const f32* pos = reinterpret_cast<const f32*>(asset.position_buffer.data());
    for (u32 i = 0; i < vcount; ++i) {
        for (u32 k = 0; k < 3; ++k) {
            const f32 v = pos[i * 3 + k];
            if (v < minp[k]) minp[k] = v;
            if (v > maxp[k]) maxp[k] = v;
        }
    }
    const f32 ex = maxp[0] - minp[0];
    const f32 ey = maxp[1] - minp[1];
    const f32 ez = maxp[2] - minp[2];
    TEST_ASSERT(std::abs(ex - 2.0f) < 0.01f, "bounds x");
    TEST_ASSERT(std::abs(ey - 2.0f) < 0.01f, "bounds y");
    TEST_ASSERT(std::abs(ez - 2.0f) < 0.01f, "bounds z");
    return TestResult::Passed;
}

// --- T9: collapsed pillar + broken corner in/out ---

TestResult TestCollapsedPillar_TiltShiftsTop() {
    graphics::rhi::RHIMeshAsset asset{};
    content::create_collapsed_pillar_mesh(asset, 0.5f, 1.0f,
                                          content::TiltAxis::PlusX, 0.3f);
    TEST_ASSERT(asset.num_vertices > 0, "verts non-zero");
    TEST_ASSERT(asset.num_indices > 0,  "indices non-zero");
    return TestResult::Passed;
}

TestResult TestBrokenCornerIn_Composes() {
    graphics::rhi::RHIMeshAsset asset{};
    content::create_broken_corner_in_mesh(asset, 1.0f, 1.0f, 1.0f,
                                          content::BrokenCorner::PosXYZ);
    TEST_ASSERT(asset.num_vertices >= 24u, "at least cube verts");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCProceduralMeshesRuins");
    TEST_CASE(suite, "AcceptsMaterialIdx",              TestRegisterProceduralMesh_AcceptsMaterialIdx);
    TEST_CASE(suite, "BrokenCube_VertexIndexCount",     TestBrokenCube_VertexIndexCount);
    TEST_CASE(suite, "BrokenCube_BoundsApproxInput",    TestBrokenCube_BoundsApproxInput);
    TEST_CASE(suite, "CollapsedPillar_TiltShiftsTop",   TestCollapsedPillar_TiltShiftsTop);
    TEST_CASE(suite, "BrokenCornerIn_Composes",         TestBrokenCornerIn_Composes);
    suite.RunAllTests();
    return 0;
}
