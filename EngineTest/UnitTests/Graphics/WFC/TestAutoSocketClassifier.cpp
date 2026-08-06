#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"

using namespace primal::math;
using namespace primal::graphics::wfc;
using namespace primal::graphics::rhi;
using namespace primal::content;
using namespace Engine::Test;

// Triangle in XY plane facing +Z, vertices CCW from +Z.
// max_t=0.1m mirrors a face-sample ray length, so origin sits 0.05m above.
TestResult TestRayTriangleHit() {
    v3 v0{-1, -1, 0}, v1{1, -1, 0}, v2{0, 1, 0};
    v3 ray_origin{0, 0, 0.05f}, ray_dir{0, 0, -1};
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT(hit, "ray should hit triangle");
    TEST_ASSERT(t > 0.0f, "t must be positive");
    TEST_ASSERT(t < 0.1f, "t must be within max_t");
    return TestResult::Passed;
}

TestResult TestRayTriangleMiss() {
    v3 v0{-1, -1, 0}, v1{1, -1, 0}, v2{0, 1, 0};
    // Ray within range but aimed off-triangle (XY offset, not Z).
    v3 ray_origin{5, 5, 0.05f}, ray_dir{0, 0, -1};
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT(!hit, "ray should miss");
    return TestResult::Passed;
}

TestResult TestRayTriangleBackfaceCull() {
    // Reverse winding (CW from +Z) — ray from +Z should miss (backface).
    v3 v0{0, 1, 0}, v1{1, -1, 0}, v2{-1, -1, 0};  // CW from +Z
    v3 ray_origin{0, 0, 0.05f}, ray_dir{0, 0, -1};
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT(!hit, "backface should be culled");
    return TestResult::Passed;
}

// --- Task 6: ClassifyFace + MirrorFlipU ---

// A solid unit cube: every face is fully solid (all 64 bits = 1).
// emit_box_geometry populates a 24-vert / 36-index cube without registration.
TestResult TestClassifyFaceSolidCube() {
    RHIMeshAsset cube;
    emit_box_geometry(cube, 1.0f, 1.0f, 1.0f);  // unit cube at origin

    const m4x4 identity = matrix_identity_float4x4;
    const SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        cube, WFCFace::PosX, identity);
    TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sig,
                   "solid cube +X face must be all ones");
    return TestResult::Passed;
}

// Empty mesh (default-constructed RHIMeshAsset has zero-sized buffers):
// every face is all-zero.
TestResult TestClassifyFaceEmptyMesh() {
    RHIMeshAsset empty;  // default-constructed: zero-sized buffers
    const m4x4 identity = matrix_identity_float4x4;
    const SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        empty, WFCFace::PosX, identity);
    TEST_ASSERT_EQ(0ULL, sig, "empty mesh face must be all zeros");
    return TestResult::Passed;
}

// MirrorFlipU reverses bits in each 8-bit row.
// Symmetric bit pattern (bit 0 and bit 7 both set in row 0) mirrors to itself.
TestResult TestMirrorFlipU() {
    SocketEncoding sig = 0;
    sig |= (1ULL << 0);   // bit (i=0, j=0) — first bit of row 0
    sig |= (1ULL << 7);   // bit (i=7, j=0) — last bit of row 0
    const SocketEncoding mirrored = AutoSocketClassifier::MirrorFlipU(sig);
    TEST_ASSERT_EQ(sig, mirrored,
                   "row symmetric pattern (bits 0 and 7) mirrors to itself");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("AutoSocketClassifier");
    TEST_CASE(suite, "RayTriangleHit",         TestRayTriangleHit);
    TEST_CASE(suite, "RayTriangleMiss",        TestRayTriangleMiss);
    TEST_CASE(suite, "RayTriangleBackfaceCull", TestRayTriangleBackfaceCull);
    TEST_CASE(suite, "ClassifyFaceSolidCube",  TestClassifyFaceSolidCube);
    TEST_CASE(suite, "ClassifyFaceEmptyMesh",  TestClassifyFaceEmptyMesh);
    TEST_CASE(suite, "MirrorFlipU",            TestMirrorFlipU);
    suite.RunAllTests();
    return 0;
}
