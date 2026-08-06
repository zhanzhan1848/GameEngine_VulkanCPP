#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::math;
using namespace primal::graphics::wfc;
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

int main() {
    TestSuite suite("AutoSocketClassifier");
    TEST_CASE(suite, "RayTriangleHit",         TestRayTriangleHit);
    TEST_CASE(suite, "RayTriangleMiss",        TestRayTriangleMiss);
    TEST_CASE(suite, "RayTriangleBackfaceCull", TestRayTriangleBackfaceCull);
    suite.RunAllTests();
    return 0;
}
