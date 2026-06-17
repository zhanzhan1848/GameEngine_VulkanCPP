#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Camera.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include <iostream>

using namespace Engine::Test;
using namespace primal::game_entity;
using namespace primal::camera;
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

TestResult TestCameraCreateAndGet()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.type = primal::graphics::camera::perspective;
    info.up = {0.0f, 1.0f, 0.0f};
    info.field_of_view = 0.5f;
    info.aspect_ratio = 2.0f;
    info.near_z = 0.5f;
    info.far_z = 100.0f;

    component cc = create(info, e);
    TEST_ASSERT(cc.is_valid(), "Camera component should be valid");

    // Verify reads match writes
    TEST_ASSERT_EQ(static_cast<u32>(primal::graphics::camera::perspective), static_cast<u32>(cc.projection_type()), "Type should match");
    TEST_ASSERT(std::abs(0.0f - cc.up_vector().x) < 0.001f, "Up X should match");
    TEST_ASSERT(std::abs(1.0f - cc.up_vector().y) < 0.001f, "Up Y should match");
    TEST_ASSERT(std::abs(0.0f - cc.up_vector().z) < 0.001f, "Up Z should match");
    TEST_ASSERT(std::abs(0.5f - cc.field_of_view()) < 0.001f, "FOV should match");
    TEST_ASSERT(std::abs(2.0f - cc.aspect_ratio()) < 0.001f, "Aspect should match");
    TEST_ASSERT(std::abs(0.5f - cc.near_z()) < 0.001f, "Near Z should match");
    TEST_ASSERT(std::abs(100.0f - cc.far_z()) < 0.001f, "Far Z should match");

    remove(cc);
    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

TestResult TestCameraSetters()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.type = primal::graphics::camera::orthographic;
    component cc = create(info, e);
    TEST_ASSERT(cc.is_valid(), "Camera component should be valid");

    cc.set_up_vector({1.0f, 0.0f, 0.0f});
    cc.set_field_of_view(0.8f);
    cc.set_aspect_ratio(1.5f);
    cc.set_range(1.0f, 200.0f);

    TEST_ASSERT(std::abs(1.0f - cc.up_vector().x) < 0.001f, "Up X after set");
    TEST_ASSERT(std::abs(0.0f - cc.up_vector().y) < 0.001f, "Up Y after set");
    TEST_ASSERT(std::abs(0.8f - cc.field_of_view()) < 0.001f, "FOV after set");
    TEST_ASSERT(std::abs(1.5f - cc.aspect_ratio()) < 0.001f, "Aspect after set");
    TEST_ASSERT(std::abs(1.0f - cc.near_z()) < 0.001f, "Near Z after set");
    TEST_ASSERT(std::abs(200.0f - cc.far_z()) < 0.001f, "Far Z after set");

    remove(cc);
    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

TestResult TestCameraViaEntityAdd()
{
    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.field_of_view = 0.3f;
    e.Add<Camera>(info);

    TEST_ASSERT(e.Has<Camera>(), "Entity should have Camera component");

    auto cc = e.Get<Camera>();
    TEST_ASSERT(cc.is_valid(), "Get<Camera> should return valid component");
    TEST_ASSERT(std::abs(0.3f - cc.field_of_view()) < 0.001f, "FOV via entity");

    e.Remove<Camera>();
    TEST_ASSERT(!e.Has<Camera>(), "Entity should not have Camera after Remove");

    remove_game_entity(e.get_id());
    return TestResult::Passed;
}

void RunCameraComponentTests()
{
    TestSuite suite("Camera Component Tests");
    suite.AddTestCase(TestCase("Create and Get", TestCameraCreateAndGet, "Create camera, verify reads match writes"));
    suite.AddTestCase(TestCase("Setters", TestCameraSetters, "Test all setter methods"));
    suite.AddTestCase(TestCase("Entity Add/Remove", TestCameraViaEntityAdd, "Test via entity template API"));
    suite.RunAllTests();
}

int main()
{
    RunCameraComponentTests();
    return 0;
}
