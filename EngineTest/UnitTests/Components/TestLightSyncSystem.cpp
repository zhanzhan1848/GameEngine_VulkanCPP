// === Phase 4.5: LightSyncSystem 集成测试 ===
// 验证 ECS Light 组件 → RenderScene::lights_ 的同步链路真正闭环。
// 这是 Phase 3+4 的"接线"测试：Phase 3 造了组件，Phase 4 让 ForwardSceneRenderer
// 读 GetLights()，Phase 4.5 把两者粘起来。本测试在单元测试层直接调 SyncLightsFromECS
// 验证数据流，不依赖完整渲染管线。

#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Light.h"
#include "Engine/Components/ComponentTraits.h"
#include "Engine/EngineAPI/GameEntity.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/Scene/LightSyncSystem.h"
#include <cmath>
#include <iostream>

using namespace Engine::Test;
using namespace primal::game_entity;
using namespace primal::light;
using namespace primal::component;
using primal::graphics::RenderScene;
using primal::graphics::RenderLight;
using primal::graphics::LightType;
using primal::graphics::scene_sync::SyncLightsFromECS;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

// Helper: create entity with transform at given position
static entity create_entity_at(f32 x, f32 y, f32 z)
{
    primal::transform::init_info tinfo{};
    tinfo.position[0] = x;
    tinfo.position[1] = y;
    tinfo.position[2] = z;
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// === Test 1: 单个 directional light 同步 ===
TestResult TestSyncOneDirectionalLight()
{
    entity e = create_entity_at(10.f, 20.f, 30.f);
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info linfo{};
    linfo.type = primal::graphics::light::directional;
    linfo.intensity = 2.5f;
    linfo.color = {0.8f, 0.6f, 0.4f};
    linfo.is_enabled = true;
    e.Add<Light>(linfo);
    TEST_ASSERT(e.Has<Light>(), "Entity should have Light component");

    RenderScene scene;
    SyncLightsFromECS(scene);

    const auto& lights = scene.GetLights();
    TEST_ASSERT_EQ(static_cast<size_t>(1), lights.size(), "Should have 1 light after sync");

    if (!lights.empty()) {
        const RenderLight& rl = lights[0];
        TEST_ASSERT_EQ(static_cast<u32>(LightType::Directional), static_cast<u32>(rl.type),
                       "Type should be Directional");
        TEST_ASSERT(std::abs(2.5f - rl.intensity) < 0.001f, "Intensity should match");
        TEST_ASSERT(std::abs(0.8f - rl.color.x) < 0.001f, "Color R should match");
        TEST_ASSERT(std::abs(0.6f - rl.color.y) < 0.001f, "Color G should match");
        TEST_ASSERT(std::abs(0.4f - rl.color.z) < 0.001f, "Color B should match");
        // Position should come from Transform (10, 20, 30).
        TEST_ASSERT(std::abs(10.f - rl.position.x) < 0.001f, "Position X should come from Transform");
        TEST_ASSERT(std::abs(20.f - rl.position.y) < 0.001f, "Position Y should come from Transform");
        TEST_ASSERT(std::abs(30.f - rl.position.z) < 0.001f, "Position Z should come from Transform");
    }

    e.Remove<Light>();
    primal::game_entity::remove(e.get_id());
    return TestResult::Passed;
}

// === Test 2: 禁用的 light 不应被同步 ===
TestResult TestDisabledLightSkipped()
{
    entity e = create_entity_at(0.f, 0.f, 0.f);
    init_info linfo{};
    linfo.intensity = 1.f;
    linfo.is_enabled = false;  // explicitly disabled
    e.Add<Light>(linfo);

    RenderScene scene;
    SyncLightsFromECS(scene);

    TEST_ASSERT_EQ(static_cast<size_t>(0), scene.GetLights().size(),
                   "Disabled light should be skipped");

    e.Remove<Light>();
    primal::game_entity::remove(e.get_id());
    return TestResult::Passed;
}

// === Test 3: 多 light 同步 + ClearLights 幂等 ===
TestResult TestSyncMultipleLightsAndClear()
{
    entity e1 = create_entity_at(1.f, 2.f, 3.f);
    init_info linfo1{};
    linfo1.type = primal::graphics::light::directional;
    linfo1.color = {1.f, 0.f, 0.f};
    e1.Add<Light>(linfo1);

    entity e2 = create_entity_at(4.f, 5.f, 6.f);
    init_info linfo2{};
    linfo2.type = primal::graphics::light::point;
    linfo2.color = {0.f, 1.f, 0.f};
    linfo2.range = 15.f;
    e2.Add<Light>(linfo2);

    RenderScene scene;
    SyncLightsFromECS(scene);
    TEST_ASSERT_EQ(static_cast<size_t>(2), scene.GetLights().size(), "Should sync 2 lights");

    // Remove e1's Light, re-sync. SyncLightsFromECS calls ClearLights internally,
    // so scene should reflect current ECS state (1 light, not 2).
    e1.Remove<Light>();
    SyncLightsFromECS(scene);
    TEST_ASSERT_EQ(static_cast<size_t>(1), scene.GetLights().size(),
                   "After removing one Light + re-sync, should have 1 light");

    if (!scene.GetLights().empty()) {
        const RenderLight& rl = scene.GetLights()[0];
        TEST_ASSERT_EQ(static_cast<u32>(LightType::Point), static_cast<u32>(rl.type),
                       "Remaining light should be Point");
        TEST_ASSERT(std::abs(15.f - rl.range) < 0.001f, "Range should match");
    }

    e2.Remove<Light>();
    primal::game_entity::remove(e1.get_id());
    primal::game_entity::remove(e2.get_id());
    return TestResult::Passed;
}

// === Test 4: 没有 Light 组件的 entity 不被同步 ===
TestResult TestEntityWithoutLightIgnored()
{
    entity e_with = create_entity_at(0.f, 0.f, 0.f);
    init_info linfo{};
    e_with.Add<Light>(linfo);

    entity e_without = create_entity_at(1.f, 1.f, 1.f);  // Transform only, no Light

    RenderScene scene;
    SyncLightsFromECS(scene);

    // Only e_with should contribute. e_without exists in entity_count() but has no Light.
    TEST_ASSERT_EQ(static_cast<size_t>(1), scene.GetLights().size(),
                   "Only entities with Light component should be synced");

    e_with.Remove<Light>();
    primal::game_entity::remove(e_with.get_id());
    primal::game_entity::remove(e_without.get_id());
    return TestResult::Passed;
}

void RunLightSyncTests()
{
    TestSuite suite("Light Sync System Tests (Phase 4.5)");
    suite.AddTestCase(TestCase("Sync One Directional Light",
        TestSyncOneDirectionalLight,
        "ECS Light + Transform → RenderScene.AddLight with correct position/color"));
    suite.AddTestCase(TestCase("Disabled Light Skipped",
        TestDisabledLightSkipped,
        "is_enabled=false lights should not be synced"));
    suite.AddTestCase(TestCase("Multiple Lights + Clear Idempotency",
        TestSyncMultipleLightsAndClear,
        "Multiple lights sync + re-sync after remove should reflect current ECS state"));
    suite.AddTestCase(TestCase("Entity Without Light Ignored",
        TestEntityWithoutLightIgnored,
        "Entities without Light component should be skipped"));
    suite.RunAllTests();
}

int main()
{
    RunLightSyncTests();
    return 0;
}
