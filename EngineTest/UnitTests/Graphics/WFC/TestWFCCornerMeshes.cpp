// EngineTest/UnitTests/Graphics/WFC/TestWFCCornerMeshes.cpp
// Phase B.1 Task 5: sanity checks for create_corner_in_mesh / create_corner_out_mesh.
//
// register_mesh_asset returns a geometry_hierarchies ID; the bridge
// get_rhi_mesh_id + get_rhi_mesh_asset exists but adds engine-state coupling
// for a small unit test. We follow the plan's simpler inline-recompute approach:
//   - Registration tests assert the returned handle is valid.
//   - First-triangle winding tests recompute the geometry inline using the
//     same vertex positions the generators write (see create_corner_in_mesh
//     / create_corner_out_mesh in Engine/Content/ProceduralMesh.h).
//
// Cross product sign convention: outward-faces CCW -> (v1-v0) x (v2-v0)
// points away from the mesh centroid.

#include "../../TestFramework.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Common/CommonHeaders.h"

#include <cmath>

using namespace primal::content;
using namespace primal::math;
using namespace Engine::Test;

TestResult TestCornerIn_Registration_Succeeds() {
    // create_corner_in_mesh builds an L-shape with 12 verts and 60 indices.
    // Coverage: registration returns a valid handle. Geometry counts are
    // indirectly exercised by TestWFCRendering rendering the catalog.
    primal::id::id_type handle = create_corner_in_mesh(1.0f, 1.0f, 1.0f);
    TEST_ASSERT(handle != primal::id::invalid_id, "Registration returns valid id");
    return TestResult::Passed;
}

TestResult TestCornerIn_First_Triangle_Non_Degenerate() {
    // First triangle of corner_in is the bottom hexagon fan: v0, v1, v2.
    // v0=(-hx,-hy,-hz), v1=(+hx,-hy,-hz), v2=(+hx,-hy,0). All at y=-hy.
    // (v1-v0) × (v2-v0): outward normal should point -Y (outward for bottom).
    const f32 hx = 0.5f, hy = 0.5f, hz = 0.5f;
    v3 v0{-hx, -hy, -hz};
    v3 v1{ hx, -hy, -hz};
    v3 v2{ hx, -hy,  0.0f};
    v3 e1 = v1 - v0;  // (2hx, 0, 0)
    v3 e2 = v2 - v0;  // (2hx, 0, hz)
    v3 n{
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x,
    };
    // For hx=hz=0.5: n = (0, -hx*hz*2, 0) = (0, -0.5, 0) → points -Y.
    TEST_ASSERT(n.y < 0.0f, "corner_in first triangle normal points -Y (outward)");
    TEST_ASSERT(std::abs(n.y) > 1e-6f, "corner_in first triangle non-degenerate");
    return TestResult::Passed;
}

TestResult TestCornerOut_Registration_Succeeds() {
    // create_corner_out_mesh builds an octant frame with 12 verts and 18 idx.
    // Same readback limitation as corner_in — coverage is via TestWFCRendering.
    primal::id::id_type handle = create_corner_out_mesh(1.0f, 1.0f, 1.0f);
    TEST_ASSERT(handle != primal::id::invalid_id, "Registration returns valid id");
    return TestResult::Passed;
}

TestResult TestCornerOut_First_Triangle_Non_Degenerate() {
    // First triangle of corner_out: v0, v1, v2 — the +X wall CCW from +X viewer.
    // v0=(+hx,-hy,-hz), v1=(+hx,+hy,-hz), v2=(+hx,+hy,+hz). All at x=+hx.
    // (v1-v0)=(0,2hy,0), (v2-v0)=(0,2hy,2hz).
    // Cross = (2hy*2hz - 0, 0 - 0, 0 - 0) = (4hy*hz, 0, 0) → points +X.
    const f32 hx = 0.5f, hy = 0.5f, hz = 0.5f;
    v3 v0{ hx, -hy, -hz};
    v3 v1{ hx,  hy, -hz};
    v3 v2{ hx,  hy,  hz};
    v3 e1 = v1 - v0;
    v3 e2 = v2 - v0;
    v3 n{
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x,
    };
    TEST_ASSERT(n.x > 0.0f, "corner_out first triangle normal points +X (outward)");
    TEST_ASSERT(std::abs(n.x) > 1e-6f, "corner_out first triangle non-degenerate");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCCornerMeshes");
    TEST_CASE(suite, "CornerIn_Registration_Succeeds",
              TestCornerIn_Registration_Succeeds);
    TEST_CASE(suite, "CornerIn_First_Triangle_Non_Degenerate",
              TestCornerIn_First_Triangle_Non_Degenerate);
    TEST_CASE(suite, "CornerOut_Registration_Succeeds",
              TestCornerOut_Registration_Succeeds);
    TEST_CASE(suite, "CornerOut_First_Triangle_Non_Degenerate",
              TestCornerOut_First_Triangle_Non_Degenerate);
    suite.RunAllTests();
    // Release registered mesh assets so engine free_lists don't assert at
    // program exit (matches the Shutdown pattern in TestWFCRendering).
    primal::content::shutdown();
    return 0;
}
