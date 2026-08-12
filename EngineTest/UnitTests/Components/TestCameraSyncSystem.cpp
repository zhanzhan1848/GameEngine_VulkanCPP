// === Phase 3: CameraSyncSystem 集成测试 ===
// 验证 ECS Camera 组件 → RenderView 的 view/projection 矩阵同步链路。
//
// 与 LightSyncSystem 测试平行的"接线"测试。重点验证：
//   1) 场景里没有 Camera entity → RenderView 保持上游 caller 设置的矩阵（fallback）
//   2) 有一个 Camera entity → RenderView 矩阵被覆盖
//   3) 多个 Camera entity → 第一个（按 entity_index 升序）胜出
//   4) Camera 移除后再 sync → RenderView 矩阵保持不变（no camera = no overwrite）
//
// 注意：Camera 组件的 view()/projection() 目前返回 identity stub；CameraSyncSystem
// 现场从 fov/aspect/near/far/projection_type + Transform world matrix 构造矩阵。
// 因此测试验证的是数据流（"矩阵确实被覆盖了"）而不是精确数值（"等于某个 hand-computed 值"）。

#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Camera.h"
#include "Engine/Components/ComponentTraits.h"
#include "Engine/EngineAPI/GameEntity.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/Scene/CameraSyncSystem.h"

#include <cmath>

using namespace Engine::Test;
using namespace primal::game_entity;
using namespace primal::camera;
using namespace primal::component;
using primal::graphics::RenderView;
using primal::graphics::scene_sync::SyncCamerasFromECS;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

// Helper: create entity with transform at given position.
// 必须显式设置 rotation 为 identity quaternion (0,0,0,1)；transform::init_info 默认
// 把 rotation[4] 零初始化成 (0,0,0,0)，simd_matrix3x3 在该输入下产生 NaN，会让
// world 矩阵的旋转列变成 NaN，进而让 Inverse(world) 整个矩阵都 NaN。
// （LightSyncSystem 没暴露这个 bug 是因为它只读 world.columns[3] 的 translation。）
static entity create_entity_at(f32 x, f32 y, f32 z)
{
    primal::transform::init_info tinfo{};
    tinfo.position[0] = x;
    tinfo.position[1] = y;
    tinfo.position[2] = z;
    tinfo.rotation[0] = 0.f;
    tinfo.rotation[1] = 0.f;
    tinfo.rotation[2] = 0.f;
    tinfo.rotation[3] = 1.f;  // identity quaternion (w=1)
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// Helper: check if two m4x4 are equal (component-wise, with epsilon).
static bool matrices_equal(const primal::math::m4x4& a, const primal::math::m4x4& b, f32 eps)
{
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            if (std::abs(a.columns[col][row] - b.columns[col][row]) > eps) return false;
        }
    }
    return true;
}

// Sentinel matrices used as "upstream-set" baselines. Distinct from any plausible
// camera-derived matrix so we can detect "sync overwrote them" vs "sync no-op'd".
static primal::math::m4x4 make_baseline_view()
{
    primal::math::m4x4 m{};
    m.columns[0] = {2.0f, 0.0f, 0.0f, 0.0f};
    m.columns[1] = {0.0f, 2.0f, 0.0f, 0.0f};
    m.columns[2] = {0.0f, 0.0f, 2.0f, 0.0f};
    m.columns[3] = {9.0f, 9.0f, 9.0f, 1.0f};
    return m;
}

static primal::math::m4x4 make_baseline_proj()
{
    primal::math::m4x4 m{};
    m.columns[0] = {3.0f, 0.0f, 0.0f, 0.0f};
    m.columns[1] = {0.0f, 3.0f, 0.0f, 0.0f};
    m.columns[2] = {0.0f, 0.0f, 3.0f, 0.0f};
    m.columns[3] = {0.0f, 0.0f, 0.0f, 1.0f};
    return m;
}

// === Test 1: 没有 Camera entity → RenderView 矩阵保持不变 ===
TestResult TestNoCameraLeavesViewUnchanged()
{
    RenderView view;
    const primal::math::m4x4 base_v = make_baseline_view();
    const primal::math::m4x4 base_p = make_baseline_proj();
    view.SetViewMatrix(base_v);
    view.SetProjectionMatrix(base_p);

    // 关键：调用前场景里不应有 Camera entity。前面其他测试如果 cleanup 干净，这里 OK。
    SyncCamerasFromECS(view);

    const bool view_unchanged = matrices_equal(view.GetViewMatrix(), base_v, 0.0001f);
    const bool proj_unchanged = matrices_equal(view.GetProjectionMatrix(), base_p, 0.0001f);
    TEST_ASSERT(view_unchanged, "View matrix should be unchanged when no Camera entity exists");
    TEST_ASSERT(proj_unchanged, "Projection matrix should be unchanged when no Camera entity exists");
    return TestResult::Passed;
}

// === Test 2: 一个 Camera entity → RenderView 矩阵被覆盖 ===
TestResult TestOneCameraOverwritesView()
{
    entity e = create_entity_at(0.f, 0.f, 5.f);
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info cinfo{};
    cinfo.type = primal::graphics::camera::perspective;
    cinfo.up = {0.0f, 1.0f, 0.0f};
    cinfo.field_of_view = 1.0f;     // ~57 degrees vertical
    cinfo.aspect_ratio = 1.5f;
    cinfo.near_z = 0.5f;
    cinfo.far_z = 100.0f;
    e.Add<Camera>(cinfo);
    TEST_ASSERT(e.Has<Camera>(), "Entity should have Camera component");

    RenderView view;
    const primal::math::m4x4 base_v = make_baseline_view();
    const primal::math::m4x4 base_p = make_baseline_proj();
    view.SetViewMatrix(base_v);
    view.SetProjectionMatrix(base_p);

    SyncCamerasFromECS(view);

    const bool view_changed = !matrices_equal(view.GetViewMatrix(), base_v, 0.0001f);
    const bool proj_changed = !matrices_equal(view.GetProjectionMatrix(), base_p, 0.0001f);
    TEST_ASSERT(view_changed, "View matrix should be overwritten by ECS Camera");
    TEST_ASSERT(proj_changed, "Projection matrix should be overwritten by ECS Camera");

    // View matrix translation column (col 3) should reflect camera position (0, 0, 5)
    // after inversion. The convention depends on transform::get_transform_matrices —
    // inverse world places camera world position with negation in col 3 of view matrix.
    // Sanity check: col 3 z-component should be non-zero (translation present).
    const f32 view_col3_z = view.GetViewMatrix().columns[3][2];
    TEST_ASSERT(std::abs(view_col3_z) > 0.001f, "View matrix translation should reflect camera position");

    // Projection matrix at (col 1, row 1) = f = 1/tan(fov/2). For fov=1.0:
    // f = 1/tan(0.5) ≈ 1.8305
    const f32 expected_f = 1.0f / std::tan(0.5f);
    const f32 proj_11 = view.GetProjectionMatrix().columns[1][1];
    TEST_ASSERT(std::abs(proj_11 - expected_f) < 0.001f,
                "Proj [1][1] should be 1/tan(fov/2)");

    e.Remove<Camera>();
    primal::game_entity::remove(e.get_id());
    return TestResult::Passed;
}

// === Test 3: 多个 Camera entity → 第一个（按 entity_index 升序）胜出 ===
TestResult TestMultipleCamerasFirstWins()
{
    entity e1 = create_entity_at(0.f, 0.f, 5.f);
    init_info cinfo1{};
    cinfo1.type = primal::graphics::camera::perspective;
    cinfo1.field_of_view = 0.5f;    // distinct FOV
    cinfo1.aspect_ratio = 1.0f;
    cinfo1.near_z = 0.5f;
    cinfo1.far_z = 100.0f;
    e1.Add<Camera>(cinfo1);

    entity e2 = create_entity_at(10.f, 0.f, 0.f);
    init_info cinfo2{};
    cinfo2.type = primal::graphics::camera::perspective;
    cinfo2.field_of_view = 1.5f;    // very different FOV
    cinfo2.aspect_ratio = 1.0f;
    cinfo2.near_z = 0.5f;
    cinfo2.far_z = 100.0f;
    e2.Add<Camera>(cinfo2);

    TEST_ASSERT(e1.get_id() != e2.get_id(), "Two entities should have different IDs");
    // e1 was created first so should have lower index (assumption: monotonic allocation).
    TEST_ASSERT(e1.get_id() < e2.get_id(), "e1 should have lower entity_id (allocated first)");

    RenderView view;
    SyncCamerasFromECS(view);

    // First camera's FOV=0.5 → f = 1/tan(0.25) ≈ 3.916
    const f32 expected_first_f = 1.0f / std::tan(0.25f);
    const f32 proj_11 = view.GetProjectionMatrix().columns[1][1];
    TEST_ASSERT(std::abs(proj_11 - expected_first_f) < 0.001f,
                "Proj [1][1] should match FIRST camera's FOV (first wins)");

    e1.Remove<Camera>();
    e2.Remove<Camera>();
    primal::game_entity::remove(e1.get_id());
    primal::game_entity::remove(e2.get_id());
    return TestResult::Passed;
}

// === Test 4: Camera 移除后再 sync → RenderView 矩阵保持不变 ===
TestResult TestCameraRemovedLeavesViewUnchanged()
{
    // 先确认开始时没有 Camera（前面测试已 cleanup）
    RenderView view;
    const primal::math::m4x4 base_v = make_baseline_view();
    const primal::math::m4x4 base_p = make_baseline_proj();
    view.SetViewMatrix(base_v);
    view.SetProjectionMatrix(base_p);

    entity e = create_entity_at(0.f, 0.f, 5.f);
    init_info cinfo{};
    cinfo.type = primal::graphics::camera::perspective;
    cinfo.field_of_view = 1.0f;
    cinfo.aspect_ratio = 1.5f;
    cinfo.near_z = 0.5f;
    cinfo.far_z = 100.0f;
    e.Add<Camera>(cinfo);

    // 第一次 sync：应当覆盖 baseline
    SyncCamerasFromECS(view);
    const bool first_sync_changed =
        !matrices_equal(view.GetViewMatrix(), base_v, 0.0001f) ||
        !matrices_equal(view.GetProjectionMatrix(), base_p, 0.0001f);
    TEST_ASSERT(first_sync_changed, "First sync with Camera present should overwrite matrices");

    // 记录 sync 后的矩阵
    const primal::math::m4x4 after_sync_v = view.GetViewMatrix();
    const primal::math::m4x4 after_sync_p = view.GetProjectionMatrix();

    // 移除 Camera，再 sync：应当不再覆盖（场景里没有 Camera entity）
    e.Remove<Camera>();
    SyncCamerasFromECS(view);

    const bool second_sync_unchanged =
        matrices_equal(view.GetViewMatrix(), after_sync_v, 0.0001f) &&
        matrices_equal(view.GetProjectionMatrix(), after_sync_p, 0.0001f);
    TEST_ASSERT(second_sync_unchanged,
                "After Camera removed, second sync should leave view unchanged (no camera = no overwrite)");

    primal::game_entity::remove(e.get_id());
    return TestResult::Passed;
}

void RunCameraSyncTests()
{
    TestSuite suite("Camera Sync System Tests (Phase 3)");
    suite.AddTestCase(TestCase("No Camera → View Unchanged",
        TestNoCameraLeavesViewUnchanged,
        "SyncCamerasFromECS no-ops when no Camera entity exists"));
    suite.AddTestCase(TestCase("One Camera → Overwrites View",
        TestOneCameraOverwritesView,
        "ECS Camera + Transform → RenderView view/proj overwritten with constructed matrices"));
    suite.AddTestCase(TestCase("Multiple Cameras → First Wins",
        TestMultipleCamerasFirstWins,
        "First Camera entity (lowest index) drives RenderView"));
    suite.AddTestCase(TestCase("Camera Removed → View Stays",
        TestCameraRemovedLeavesViewUnchanged,
        "After Remove<Camera>(), next sync leaves RenderView matrices unchanged"));
    suite.RunAllTests();
}

int main()
{
    RunCameraSyncTests();
    return 0;
}
