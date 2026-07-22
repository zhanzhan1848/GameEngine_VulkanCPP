#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Light.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include <iostream>

using namespace Engine::Test;
using namespace primal::game_entity;
using namespace primal::light;
using namespace primal::component;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;
using Engine::Test::TestStats;

entity create_one_game_entity()
{
    primal::transform::init_info transform_info{};
    primal::game_entity::entity_info entity_info{};
    entity_info.transform = &transform_info;
    return primal::game_entity::create(entity_info);
}

void remove_game_entity(entity_id id)
{
    primal::game_entity::remove(id);
}

TestResult TestLightCreateAndGet()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.type = primal::graphics::light::directional;
    info.intensity = 2.5f;
    info.color = {1.0f, 0.5f, 0.2f};
    info.is_enabled = true;

    component lc = create(info, e);
    TEST_ASSERT(lc.is_valid(), "Light component should be valid");

    // Verify reads match writes
    TEST_ASSERT_EQ(static_cast<u32>(primal::graphics::light::directional), static_cast<u32>(lc.light_type()), "Type should match");
    TEST_ASSERT(std::abs(2.5f - lc.intensity()) < 0.001f, "Intensity should match");
    TEST_ASSERT(std::abs(1.0f - lc.color().x) < 0.001f, "Color R should match");
    TEST_ASSERT(std::abs(0.5f - lc.color().y) < 0.001f, "Color G should match");
    TEST_ASSERT(std::abs(0.2f - lc.color().z) < 0.001f, "Color B should match");
    TEST_ASSERT(lc.is_enabled(), "Should be enabled");

    remove(lc);
    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

TestResult TestLightSetters()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.type = primal::graphics::light::point;
    component lc = create(info, e);
    TEST_ASSERT(lc.is_valid(), "Light component should be valid");

    // Test setters
    lc.set_intensity(5.0f);
    lc.set_color({0.1f, 0.2f, 0.3f});
    lc.set_range(25.0f);
    lc.set_cone_angles(0.5f, 1.0f);
    lc.set_enabled(false);
    lc.set_light_type(primal::graphics::light::spot);

    TEST_ASSERT(std::abs(5.0f - lc.intensity()) < 0.001f, "Intensity after set");
    TEST_ASSERT(std::abs(0.1f - lc.color().x) < 0.001f, "Color R after set");
    TEST_ASSERT(std::abs(0.2f - lc.color().y) < 0.001f, "Color G after set");
    TEST_ASSERT(std::abs(0.3f - lc.color().z) < 0.001f, "Color B after set");
    TEST_ASSERT(std::abs(25.0f - lc.range()) < 0.001f, "Range after set");
    TEST_ASSERT(std::abs(0.5f - lc.umbra()) < 0.001f, "Umbra after set");
    TEST_ASSERT(std::abs(1.0f - lc.penumbra()) < 0.001f, "Penumbra after set");
    TEST_ASSERT(!lc.is_enabled(), "Should be disabled");
    TEST_ASSERT_EQ(static_cast<u32>(primal::graphics::light::spot), static_cast<u32>(lc.light_type()), "Type should be spot");

    remove(lc);
    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

TestResult TestLightViaEntityAdd()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.intensity = 10.0f;
    e.Add<Light>(info);

    TEST_ASSERT(e.Has<Light>(), "Entity should have Light component");

    auto lc = e.Get<Light>();
    TEST_ASSERT(lc.is_valid(), "Get<Light> should return valid component");
    TEST_ASSERT(std::abs(10.0f - lc.intensity()) < 0.001f, "Intensity via entity");

    e.Remove<Light>();
    TEST_ASSERT(!e.Has<Light>(), "Entity should not have Light after Remove");

    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

void RunLightComponentTests()
{
    TestSuite suite("Light Component Tests");
    suite.AddTestCase(TestCase("Create and Get", TestLightCreateAndGet, "Create light, verify reads match writes"));
    suite.AddTestCase(TestCase("Setters", TestLightSetters, "Test all setter methods"));
    suite.AddTestCase(TestCase("Entity Add/Remove", TestLightViaEntityAdd, "Test via entity template API"));
    suite.RunAllTests();
}

int main()
{
    RunLightComponentTests();
    return 0;
}
