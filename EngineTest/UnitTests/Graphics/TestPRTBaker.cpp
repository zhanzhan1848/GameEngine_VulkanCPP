#include "../TestFramework.h"
#include "Graphics/Utilities/PRTBaker.h"
#include "Graphics/Utilities/BVH.hpp"

using namespace primal::graphics::utl;
using namespace primal::math;

Engine::Test::TestResult TestBVHIntersection()
{
    // Create a single triangle
    primal::utl::vector<primal::math::v3> vertices;
    vertices.push_back(primal::math::v3{-1.0f, 0.0f, -1.0f});
    vertices.push_back(primal::math::v3{ 1.0f, 0.0f, -1.0f});
    vertices.push_back(primal::math::v3{ 0.0f, 0.0f,  1.0f});

    primal::utl::vector<u32> indices;
    indices.push_back(0);
    indices.push_back(1);
    indices.push_back(2);

    BVH bvh;
    bvh.Build(vertices, indices);

    // Test hit
    BVH::Ray ray_hit(primal::math::v3{0.0f, 1.0f, 0.0f}, primal::math::v3{0.0f, -1.0f, 0.0f});
    BVH::HitInfo hit;
    bool result = bvh.Intersect(ray_hit, hit);
    
    TEST_ASSERT(result, "Ray should hit the triangle");
    TEST_ASSERT(is_equal(hit.t, 1.0f), "Hit distance should be 1.0");

    // Test miss
    BVH::Ray ray_miss(primal::math::v3{2.0f, 1.0f, 0.0f}, primal::math::v3{0.0f, -1.0f, 0.0f});
    result = bvh.Intersect(ray_miss, hit);
    TEST_ASSERT(!result, "Ray should miss the triangle");

    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestPRTBaking_Unoccluded()
{
    // Single vertex plane, pointing up
    // We use a dummy BVH (empty) by passing empty indices
    // This simulates an isolated point in space (no self-occlusion)
    
    v3 pos = {0.0f, 0.0f, 0.0f};
    v3 normal = {0.0f, 1.0f, 0.0f}; // Up
    u32 indices_dummy[] = {0, 0, 0}; // Dummy
    
    PRTBakingDesc desc;
    desc.positions = &pos;
    desc.normals = &normal;
    desc.vertex_count = 1;
    desc.indices = indices_dummy;
    desc.index_count = 0; // Empty geometry
    desc.num_samples = 8192; // Higher samples for better convergence

    primal::utl::vector<sh::SH9> results;
    bool success = PRTBaker::BakeShadowedTransfer(desc, results);

    TEST_ASSERT(success, "Baking should succeed");
    TEST_ASSERT(results.size() == 1, "Should have 1 result");

    // Theoretical Irradiance for Cosine Lobe (Lambertian surface)
    // Irradiance E = PI * L (if L is constant 1.0)
    // We are baking Transfer T.
    // T dot L_env should give E.
    // If L_env is uniform 1.0 (SH coeffs: L0=sqrt(4pi), others=0).
    // Wait, uniform 1.0 in SH:
    // L00 = integral(1.0 * Y00) = integral(Y00) = sqrt(4pi) * Y00 integral(Y00/sqrt(4pi))?
    // L00 = integral(1.0 * 1/sqrt(4pi) * sqrt(4pi)/sqrt(4pi))?
    // L00 = 2*sqrt(pi) = 3.5449
    
    // If we convolve T with uniform 1.0 environment:
    // E = dot(T, L_env) = T0 * L00 (since L_env has only L0 term)
    // For unoccluded surface with normal up, E should be PI (if environment is 1.0).
    // So T0 * 3.5449 = PI
    // T0 = PI / 3.5449 = 3.14159 / 3.5449 = 0.8862
    
    // Let's check L0 coefficient of T.
    float t0 = results[0].coeffs[0];
    
    // Expected T0: 0.8862
    // My previous derivation: projection of max(0, z) onto SH.
    // Y00 = 0.282
    // integral(max(0,z) * Y00) = Y00 * integral(cos theta * sin theta dtheta dphi)
    // integral(0..2pi) dphi * integral(0..pi/2) cos theta sin theta dtheta
    // = 2pi * [0.5 * sin^2 theta]0..pi/2 = 2pi * 0.5 = pi
    // Coeff = Y00 * pi = 0.282 * 3.14159 = 0.8862
    
    TEST_ASSERT(is_equal(t0, 0.8862f, 0.05f), "L0 coefficient should match theoretical value");

    return Engine::Test::TestResult::Passed;
}

namespace primal::test
{
    void RegisterPRTTests(Engine::Test::TestSuite& suite)
    {
        suite.AddTestCase(Engine::Test::TestCase("BVH Intersection", TestBVHIntersection));
        suite.AddTestCase(Engine::Test::TestCase("PRT Baking Unoccluded", TestPRTBaking_Unoccluded));
    }
}

int main()
{
    Engine::Test::TestSuite suite("PRT Baker Tests");
    primal::test::RegisterPRTTests(suite);
    suite.RunAllTests();
    return 0;
}
