#include "CommonHeaders.h"
#include "Graphics/RenderScene.h"
#include "TestFramework.h"
#include <iostream>

using namespace primal::graphics;
using namespace Engine::Test;

// --- Test Cases ---

TestResult TestRenderSceneLightAddRemove() {
    RenderScene scene;
    
    RenderLight l1;
    l1.entityId = 101;
    l1.type = LightType::Directional;
    l1.color = primal::math::v3{1.0f, 0.0f, 0.0f};
    l1.intensity = 10000.0f;
    
    RenderLight l2;
    l2.entityId = 102;
    l2.type = LightType::Point;
    l2.color = primal::math::v3{0.0f, 1.0f, 0.0f};
    l2.intensity = 500.0f;
    l2.range = 10.0f;
    
    scene.AddLight(l1);
    scene.AddLight(l2);
    
    TEST_ASSERT(scene.GetLights().size() == 2, "Should have 2 lights");
    
    scene.RemoveLight(101);
    TEST_ASSERT(scene.GetLights().size() == 1, "Should have 1 light after removal");
    TEST_ASSERT(scene.GetLights()[0].entityId == 102, "Remaining light should be 102");
    
    scene.Clear();
    TEST_ASSERT(scene.GetLights().empty(), "Scene should be empty after clear");
    
    return TestResult::Passed;
}

TestResult TestRenderSceneLightUpdate() {
    RenderScene scene;
    
    RenderLight l1;
    l1.entityId = 101;
    l1.type = LightType::Directional;
    l1.color = primal::math::v3{1.0f, 0.0f, 0.0f};
    
    scene.AddLight(l1);
    
    // Update color
    RenderLight l1_new = l1;
    l1_new.color = primal::math::v3{0.0f, 0.0f, 1.0f};
    
    scene.UpdateLight(101, l1_new);
    
    TEST_ASSERT(scene.GetLights().size() == 1, "Count should remain 1");
    // Verify color update (check blue channel)
    TEST_ASSERT(scene.GetLights()[0].color.z == 1.0f, "Color should be updated to blue");
    
    return TestResult::Passed;
}

TestResult TestRenderSceneLightUpdateNonExistent() {
    RenderScene scene;
    
    RenderLight l1;
    l1.entityId = 101;
    
    // Update a light that doesn't exist - should add it
    scene.UpdateLight(101, l1);
    
    TEST_ASSERT(scene.GetLights().size() == 1, "Should add non-existent light on update");
    TEST_ASSERT(scene.GetLights()[0].entityId == 101, "Added light should have correct ID");
    
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("RenderScene Light Tests");
    TEST_CASE((*suite), "TestRenderSceneLightAddRemove", TestRenderSceneLightAddRemove);
    TEST_CASE((*suite), "TestRenderSceneLightUpdate", TestRenderSceneLightUpdate);
    TEST_CASE((*suite), "TestRenderSceneLightUpdateNonExistent", TestRenderSceneLightUpdateNonExistent);
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
