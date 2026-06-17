// TestLightCameraAPI.cpp - Phase 5 ECS Light/Camera C ABI round-trip.
//
// Links EngineDLL directly and exercises LightAPI / CameraAPI / EntityAPI to
// verify the full Editor path:
//   CreateEntity → AddEntityLight → SetEntityLightIntensity → GetEntityLightIntensity
//   CreateEntity → AddEntityCamera → SetEntityCameraFov → GetEntityCameraFov
//
// These tests don't drive rendering (no surface/device required) — they verify
// the ECS component round-trip through the C ABI boundary, which is the piece
// Phase 5 adds on top of Phase 3's component storage.

#include "../UnitTests/TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"

using namespace primal;

// EngineDLL C ABI (linked directly via EngineDLL target)
extern "C" {
    u32 CreateEntity(f32 x, f32 y, f32 z);
    void DestroyEntity(u32 entity_id);

    // LightAPI
    u32 AddEntityLight(id::id_type entity_id, u32 light_type, f32 intensity,
                       const f32* color_rgb, f32 range,
                       f32 umbra_rad, f32 penumbra_rad, u32 is_enabled);
    void RemoveEntityLight(id::id_type entity_id);
    u32 HasEntityLight(id::id_type entity_id);
    u32 GetEntityLightType(id::id_type entity_id, u32* out);
    u32 GetEntityLightIntensity(id::id_type entity_id, f32* out);
    u32 GetEntityLightColor(id::id_type entity_id, f32* out_rgb);
    u32 GetEntityLightRange(id::id_type entity_id, f32* out);
    u32 GetEntityLightConeAngles(id::id_type entity_id, f32* u, f32* p);
    u32 IsEntityLightEnabled(id::id_type entity_id, u32* out);
    u32 SetEntityLightType(id::id_type entity_id, u32 t);
    u32 SetEntityLightIntensity(id::id_type entity_id, f32 v);
    u32 SetEntityLightColor(id::id_type entity_id, const f32* rgb);
    u32 SetEntityLightRange(id::id_type entity_id, f32 v);
    u32 SetEntityLightConeAngles(id::id_type entity_id, f32 u, f32 p);
    u32 SetEntityLightEnabled(id::id_type entity_id, u32 e);

    // CameraAPI
    u32 AddEntityCamera(id::id_type entity_id, u32 proj_type, f32 fov,
                        f32 aspect, f32 near_z, f32 far_z, const f32* up);
    void RemoveEntityCamera(id::id_type entity_id);
    u32 HasEntityCamera(id::id_type entity_id);
    u32 GetEntityCameraFov(id::id_type entity_id, f32* out);
    u32 GetEntityCameraAspectRatio(id::id_type entity_id, f32* out);
    u32 GetEntityCameraRange(id::id_type entity_id, f32* n, f32* f);
    u32 GetEntityCameraUp(id::id_type entity_id, f32* out);
    u32 SetEntityCameraFov(id::id_type entity_id, f32 v);
    u32 SetEntityCameraAspectRatio(id::id_type entity_id, f32 v);
    u32 SetEntityCameraRange(id::id_type entity_id, f32 n, f32 f);
    u32 SetEntityCameraUp(id::id_type entity_id, const f32* up);
}

using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

constexpr id::id_type INVALID = id::invalid_id;

// === Test 1: AddEntityLight round-trip with all fields ===
TestResult TestAddLightAndReadback()
{
    u32 e = CreateEntity(1.f, 2.f, 3.f);
    TEST_ASSERT(e != INVALID, "CreateEntity should return valid id");

    f32 color[] = {0.8f, 0.6f, 0.4f};
    u32 ok = AddEntityLight(e, /*point*/1, 2.5f, color, 15.f, 0.5f, 1.0f, /*enabled*/1);
    TEST_ASSERT_EQ(static_cast<u32>(1), ok, "AddEntityLight should return 1");
    TEST_ASSERT_EQ(static_cast<u32>(1), HasEntityLight(e), "HasEntityLight should be 1");

    u32 type = 999;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityLightType(e, &type), "GetEntityLightType");
    TEST_ASSERT_EQ(static_cast<u32>(1), type, "Type should be point(1)");

    f32 intensity = -1.f;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityLightIntensity(e, &intensity), "Get");
    TEST_ASSERT(std::abs(2.5f - intensity) < 0.001f, "Intensity round-trip");

    f32 rgb[3] = {0, 0, 0};
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityLightColor(e, rgb), "Get color");
    TEST_ASSERT(std::abs(0.8f - rgb[0]) < 0.001f, "Color R");
    TEST_ASSERT(std::abs(0.6f - rgb[1]) < 0.001f, "Color G");
    TEST_ASSERT(std::abs(0.4f - rgb[2]) < 0.001f, "Color B");

    f32 range = -1.f;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityLightRange(e, &range), "Get range");
    TEST_ASSERT(std::abs(15.f - range) < 0.001f, "Range round-trip");

    f32 umbra = -1.f, penumbra = -1.f;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityLightConeAngles(e, &umbra, &penumbra), "Get cone");
    TEST_ASSERT(std::abs(0.5f - umbra) < 0.001f, "Umbra");
    TEST_ASSERT(std::abs(1.0f - penumbra) < 0.001f, "Penumbra");

    u32 enabled = 999;
    TEST_ASSERT_EQ(static_cast<u32>(1), IsEntityLightEnabled(e, &enabled), "Get enabled");
    TEST_ASSERT_EQ(static_cast<u32>(1), enabled, "Should be enabled");

    RemoveEntityLight(e);
    TEST_ASSERT_EQ(static_cast<u32>(0), HasEntityLight(e), "HasEntityLight should be 0 after remove");

    DestroyEntity(e);
    return TestResult::Passed;
}

// === Test 2: Setters mutate state ===
TestResult TestLightSetters()
{
    u32 e = CreateEntity(0, 0, 0);
    f32 black[] = {0.f, 0.f, 0.f};
    AddEntityLight(e, 0 /*directional*/, 1.f, black, 10.f, 0.f, 0.f, 1);

    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightType(e, 2 /*spot*/), "Set type");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightIntensity(e, 7.5f), "Set intensity");
    f32 newcolor[] = {0.1f, 0.2f, 0.3f};
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightColor(e, newcolor), "Set color");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightRange(e, 42.f), "Set range");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightConeAngles(e, 0.3f, 0.7f), "Set cone");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityLightEnabled(e, 0), "Disable");

    u32 type = 999; GetEntityLightType(e, &type);
    TEST_ASSERT_EQ(static_cast<u32>(2), type, "Type after set");

    f32 intensity = 0; GetEntityLightIntensity(e, &intensity);
    TEST_ASSERT(std::abs(7.5f - intensity) < 0.001f, "Intensity after set");

    f32 rgb[3]; GetEntityLightColor(e, rgb);
    TEST_ASSERT(std::abs(0.1f - rgb[0]) < 0.001f, "R after set");
    TEST_ASSERT(std::abs(0.2f - rgb[1]) < 0.001f, "G after set");
    TEST_ASSERT(std::abs(0.3f - rgb[2]) < 0.001f, "B after set");

    f32 range = 0; GetEntityLightRange(e, &range);
    TEST_ASSERT(std::abs(42.f - range) < 0.001f, "Range after set");

    u32 enabled = 99; IsEntityLightEnabled(e, &enabled);
    TEST_ASSERT_EQ(static_cast<u32>(0), enabled, "Should be disabled");

    RemoveEntityLight(e);
    DestroyEntity(e);
    return TestResult::Passed;
}

// === Test 3: Invalid entity / missing component returns 0 ===
TestResult TestLightInvalidCases()
{
    // AddEntityLight on invalid id
    TEST_ASSERT_EQ(static_cast<u32>(0),
                   AddEntityLight(INVALID, 0, 1.f, nullptr, 10.f, 0, 0, 1),
                   "Add on invalid id should fail");

    // Getters on entity without Light component
    u32 e = CreateEntity(0, 0, 0);
    f32 dummy_f = 99.f;
    u32 dummy_u = 99;
    TEST_ASSERT_EQ(static_cast<u32>(0), GetEntityLightIntensity(e, &dummy_f), "Get on no-Light");
    TEST_ASSERT_EQ(static_cast<u32>(0), GetEntityLightType(e, &dummy_u), "Get type on no-Light");
    TEST_ASSERT_EQ(static_cast<u32>(0), HasEntityLight(e), "HasEntityLight on no-Light");

    // Setters on entity without Light
    TEST_ASSERT_EQ(static_cast<u32>(0), SetEntityLightIntensity(e, 5.f), "Set on no-Light");

    // AddEntityLight twice on same entity — second call returns 0
    f32 black[] = {0, 0, 0};
    TEST_ASSERT_EQ(static_cast<u32>(1), AddEntityLight(e, 0, 1.f, black, 10.f, 0, 0, 1), "First add");
    TEST_ASSERT_EQ(static_cast<u32>(0), AddEntityLight(e, 0, 1.f, black, 10.f, 0, 0, 1), "Second add");

    // Invalid light_type
    u32 e2 = CreateEntity(0, 0, 0);
    TEST_ASSERT_EQ(static_cast<u32>(0),
                   AddEntityLight(e2, /*out of range*/99, 1.f, black, 10.f, 0, 0, 1),
                   "Bad light_type should fail");

    DestroyEntity(e);
    DestroyEntity(e2);
    return TestResult::Passed;
}

// === Test 4: AddEntityCamera round-trip ===
TestResult TestAddCameraAndReadback()
{
    u32 e = CreateEntity(5.f, 5.f, 5.f);
    f32 up[] = {0.f, 1.f, 0.f};
    u32 ok = AddEntityCamera(e, /*perspective*/0, 0.5f, 1.78f, 0.1f, 100.f, up);
    TEST_ASSERT_EQ(static_cast<u32>(1), ok, "AddEntityCamera should return 1");
    TEST_ASSERT_EQ(static_cast<u32>(1), HasEntityCamera(e), "HasEntityCamera");

    f32 fov = 0;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityCameraFov(e, &fov), "Get fov");
    TEST_ASSERT(std::abs(0.5f - fov) < 0.001f, "FOV round-trip");

    f32 aspect = 0;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityCameraAspectRatio(e, &aspect), "Get aspect");
    TEST_ASSERT(std::abs(1.78f - aspect) < 0.001f, "Aspect round-trip");

    f32 n = 0, f = 0;
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityCameraRange(e, &n, &f), "Get range");
    TEST_ASSERT(std::abs(0.1f - n) < 0.001f, "Near");
    TEST_ASSERT(std::abs(100.f - f) < 0.001f, "Far");

    f32 up_out[3] = {0, 0, 0};
    TEST_ASSERT_EQ(static_cast<u32>(1), GetEntityCameraUp(e, up_out), "Get up");
    TEST_ASSERT(std::abs(1.f - up_out[1]) < 0.001f, "Up Y=1");

    RemoveEntityCamera(e);
    TEST_ASSERT_EQ(static_cast<u32>(0), HasEntityCamera(e), "HasEntityCamera after remove");

    DestroyEntity(e);
    return TestResult::Passed;
}

// === Test 5: Camera setters ===
TestResult TestCameraSetters()
{
    u32 e = CreateEntity(0, 0, 0);
    AddEntityCamera(e, 0, 0.25f, 16.f/9.f, 0.1f, 64.f, nullptr);

    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityCameraFov(e, 1.2f), "Set fov");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityCameraAspectRatio(e, 2.0f), "Set aspect");
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityCameraRange(e, 0.5f, 200.f), "Set range");
    f32 up[] = {1.f, 0.f, 0.f};
    TEST_ASSERT_EQ(static_cast<u32>(1), SetEntityCameraUp(e, up), "Set up");

    f32 fov = 0; GetEntityCameraFov(e, &fov);
    TEST_ASSERT(std::abs(1.2f - fov) < 0.001f, "FOV after set");

    f32 aspect = 0; GetEntityCameraAspectRatio(e, &aspect);
    TEST_ASSERT(std::abs(2.0f - aspect) < 0.001f, "Aspect after set");

    f32 n = 0, f = 0; GetEntityCameraRange(e, &n, &f);
    TEST_ASSERT(std::abs(0.5f - n) < 0.001f, "Near after set");
    TEST_ASSERT(std::abs(200.f - f) < 0.001f, "Far after set");

    f32 up_out[3]; GetEntityCameraUp(e, up_out);
    TEST_ASSERT(std::abs(1.f - up_out[0]) < 0.001f, "Up X after set");

    RemoveEntityCamera(e);
    DestroyEntity(e);
    return TestResult::Passed;
}

void RunLightCameraAPITests()
{
    TestSuite suite("Light/Camera API C ABI Tests (Phase 5)");
    suite.AddTestCase(TestCase("Add Light + Readback",
        TestAddLightAndReadback,
        "AddEntityLight → all 7 getters return what was written"));
    suite.AddTestCase(TestCase("Light Setters",
        TestLightSetters,
        "All SetEntityLight* mutate state"));
    suite.AddTestCase(TestCase("Light Invalid Cases",
        TestLightInvalidCases,
        "Invalid id / missing component / double-add / bad type all fail"));
    suite.AddTestCase(TestCase("Add Camera + Readback",
        TestAddCameraAndReadback,
        "AddEntityCamera → all getters return what was written"));
    suite.AddTestCase(TestCase("Camera Setters",
        TestCameraSetters,
        "All SetEntityCamera* mutate state"));
    suite.RunAllTests();
}

int main()
{
    RunLightCameraAPITests();
    return 0;
}
