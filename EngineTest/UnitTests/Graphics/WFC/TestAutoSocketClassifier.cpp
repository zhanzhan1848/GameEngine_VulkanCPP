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

// ClassifyTile wraps ClassifyFace for all 6 faces. A solid unit cube must
// produce all-ones signatures on every face.
TestResult TestClassifyTileReturnsSixFaces() {
    RHIMeshAsset cube;
    emit_box_geometry(cube, 1.0f, 1.0f, 1.0f);

    AutoSocketClassifier::FaceSignatures sigs =
        AutoSocketClassifier::ClassifyTile(cube, matrix_identity_float4x4);
    for (u32 f = 0; f < 6; ++f) {
        TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sigs.face[f], "all faces solid");
    }
    return TestResult::Passed;
}

// Doorway cube: +X face is a picture frame (solid border, 4x4 opening in the
// center). ClassifyFace must read corners solid and center cells as opening,
// proving the classifier detects holes — not just solid surfaces.
TestResult TestClassifyDoorwayCube() {
    RHIMeshAsset doorway;
    emit_doorway_cube_geometry(doorway, 1.0f, 1.0f, 1.0f);

    const SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        doorway, WFCFace::PosX, matrix_identity_float4x4);

    // Corners solid (row 0/7, col 0/7)
    TEST_ASSERT((sig & (SocketEncoding{1} << 0))  != 0, "(0,0) solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 7))  != 0, "(7,0) solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 56)) != 0, "(0,7) solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 63)) != 0, "(7,7) solid");
    // Center 4x4 opening (rows 2-5, cols 2-5)
    TEST_ASSERT((sig & (SocketEncoding{1} << (3 + 3*8))) == 0, "(3,3) opening");
    TEST_ASSERT((sig & (SocketEncoding{1} << (4 + 4*8))) == 0, "(4,4) opening");
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
    TEST_CASE(suite, "ClassifyTileReturnsSixFaces", TestClassifyTileReturnsSixFaces);
    TEST_CASE(suite, "ClassifyDoorwayCube",    TestClassifyDoorwayCube);
    suite.RunAllTests();
    return 0;
}
