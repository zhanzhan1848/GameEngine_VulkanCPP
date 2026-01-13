#include "CommonHeaders.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "TestFramework.h"
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// --- Test Cases ---

TestResult TestRenderSceneAddRemove() {
    RenderScene scene;
    
    RenderProxy p1 = RenderProxy::Create(101, 201, 301);
    RenderProxy p2 = RenderProxy::Create(102, 202, 302);
    
    scene.AddProxy(p1);
    scene.AddProxy(p2);
    
    TEST_ASSERT(scene.GetProxies().size() == 2, "Should have 2 proxies");
    
    scene.RemoveProxy(101);
    TEST_ASSERT(scene.GetProxies().size() == 1, "Should have 1 proxy after removal");
    TEST_ASSERT(scene.GetProxies()[0].entityId == 102, "Remaining proxy should be 102");
    
    scene.Clear();
    TEST_ASSERT(scene.GetProxies().empty(), "Scene should be empty after clear");
    
    return TestResult::Passed;
}

TestResult TestRenderSceneUpdate() {
    RenderScene scene;
    RenderProxy p1 = RenderProxy::Create(101, 201, 301);
    scene.AddProxy(p1);
    
    // Update transform
    math::m4x4 newTransform = math::CreateTranslationMatrix(math::v3{10.f, 0.f, 0.f});
    RenderProxy p1_new = p1;
    p1_new.transform = newTransform;
    
    scene.UpdateProxy(101, p1_new);
    
    TEST_ASSERT(scene.GetProxies().size() == 1, "Count should remain 1");
    // Verify transform update (check translation x)
    TEST_ASSERT(scene.GetProxies()[0].transform.columns[3][0] == 10.f, "Transform should be updated");
    
    return TestResult::Passed;
}

TestResult TestRenderSceneCulling() {
    RenderScene scene;
    
    // Proxy at origin (0,0,0) - Should be visible
    RenderProxy p1 = RenderProxy::Create(1, 1, 1);
    p1.transform = math::CreateTranslationMatrix(math::v3{0.f, 0.f, 0.f});
    // Set a valid AABB for p1 (small box around origin)
    p1.worldAABB = AABB(math::v3{-1.f, -1.f, -1.f}, math::v3{1.f, 1.f, 1.f});
    scene.AddProxy(p1);
    
    // Proxy far away (0, 0, -100) - Should be culled
    RenderProxy p2 = RenderProxy::Create(2, 2, 2);
    p2.transform = math::CreateTranslationMatrix(math::v3{0.f, 0.f, -100.f});
    p2.worldAABB = AABB(math::v3{-1.f, -1.f, -101.f}, math::v3{1.f, 1.f, -99.f});
    scene.AddProxy(p2);
    
    // Proxy behind camera (0, 0, 20) - Should be culled
    RenderProxy p3 = RenderProxy::Create(3, 3, 3);
    p3.transform = math::CreateTranslationMatrix(math::v3{0.f, 0.f, 20.f});
    p3.worldAABB = AABB(math::v3{-1.f, -1.f, 19.f}, math::v3{1.f, 1.f, 21.f});
    scene.AddProxy(p3);

    Frustum frustum;
    // Construct a ViewProjection matrix that covers [-10, 10] in X, Y, Z
    math::m4x4 vp;
    // Identity * 0.1 scale
    vp.columns[0] = math::v4{0.1f, 0.f, 0.f, 0.f};
    vp.columns[1] = math::v4{0.f, 0.1f, 0.f, 0.f};
    vp.columns[2] = math::v4{0.f, 0.f, 0.1f, 0.f};
    vp.columns[3] = math::v4{0.f, 0.f, 0.f, 1.f};

    frustum.FromMatrix(vp);

    auto visible = scene.Cull(frustum);

    bool foundP1 = false;
    bool foundP2 = false;
    bool foundP3 = false;

    for (const auto* p : visible) {
        if (p->entityId == 1) foundP1 = true;
        if (p->entityId == 2) foundP2 = true;
        if (p->entityId == 3) foundP3 = true;
    }

    TEST_ASSERT(foundP1, "P1 should be visible (inside [-10, 10])");
    TEST_ASSERT(!foundP2, "P2 should be culled (at -100)");
    TEST_ASSERT(!foundP3, "P3 should be culled (at 20)");
    
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("RenderScene Tests");
    TEST_CASE((*suite), "TestRenderSceneAddRemove", TestRenderSceneAddRemove);
    TEST_CASE((*suite), "TestRenderSceneUpdate", TestRenderSceneUpdate);
    TEST_CASE((*suite), "TestRenderSceneCulling", TestRenderSceneCulling);
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
