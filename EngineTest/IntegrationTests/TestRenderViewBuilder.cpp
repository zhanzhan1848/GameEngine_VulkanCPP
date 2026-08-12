/**
 * @file TestRenderViewBuilder.cpp
 * @brief Task 1 Integration Test: BuildRenderViewFromCameraId helper
 * @details Validates that BuildRenderViewFromCameraId correctly:
 *   1. Returns a default-constructed view for invalid camera_id (graceful path)
 *   2. Pulls view/projection matrices from the engine camera registry and
 *      populates the RenderView with them (when a Metal device is available)
 *
 * Link pattern: direct link against libEngine.a (mirrors TestRendererRHIDevice).
 * Engine init uses the three-step sequence (initialize_with_device +
 * bind_rhi_device_to_legacy + initialize(metal)); if Metal or shaders are
 * unavailable the test gracefully skips (returns 0) so it can run in CI
 * environments without a GPU.
 */

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/EngineAPI/Camera.h"
#include "Engine/EngineAPI/GameEntity.h"
#include "Engine/Graphics/Camera/RenderViewBuilder.h"
#include "Engine/Graphics/Renderer.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

#include <cassert>
#include <iostream>

using namespace primal;
using namespace primal::graphics;

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << (msg) << " (line " << __LINE__ << ")" << std::endl; \
            ++g_failures; \
        } else { \
            std::cout << "[PASS] " << (msg) << std::endl; \
        } \
    } while (0)

// Case 1: Invalid camera_id should yield a default-constructed RenderView
// (identity matrices, default viewport). This path does not need an
// initialized engine — the helper only checks id validity.
static void TestInvalidCameraIdReturnsDefault() {
    std::cout << "\n--- TestInvalidCameraIdReturnsDefault ---" << std::endl;

    rhi::ViewportDesc vp{};
    vp.topLeft  = math::v2{0.f, 0.f};
    vp.size     = math::v2{64.f, 64.f};
    vp.minDepth = 0.f;
    vp.maxDepth = 1.f;

    RenderView view = BuildRenderViewFromCameraId(camera_id{id::invalid_id}, vp);

    // Default-constructed RenderView has identity matrices.
    const math::m4x4& viewM = view.GetViewMatrix();
    const bool isIdentityView =
        viewM.columns[0][0] == 1.f && viewM.columns[1][1] == 1.f &&
        viewM.columns[2][2] == 1.f && viewM.columns[3][3] == 1.f;
    CHECK(isIdentityView, "View matrix identity for invalid camera_id");

    // Viewport should still be honored even on the early-out path.
    CHECK(view.GetViewport().size.x == 64.f, "Viewport size.x preserved for invalid id");
    CHECK(view.GetViewport().size.y == 64.f, "Viewport size.y preserved for invalid id");
    CHECK(view.GetType() == ViewType::Main, "Default ViewType is Main");
}

// Case 2: With a live camera at the origin, the view matrix translation
// column should be zero. Requires a fully initialized engine (Metal device +
// shaders). Skipped if engine init fails.
static void TestLiveCameraAtOrigin() {
    std::cout << "\n--- TestLiveCameraAtOrigin ---" << std::endl;

    // Create a game_entity for the camera to attach to. The camera backend
    // (metal_camera::update) reads entity.transform().position()/.orientation(),
    // so the entity MUST have a transform component. We use identity transform
    // (origin, identity quaternion {0,0,0,1}) so the view matrix has zero
    // translation and the camera looks down +Z (the default orientation).
    transform::init_info transform_info{};
    const math::v3 origin{0.f, 0.f, 0.f};
    const math::v4 identity_quat{0.f, 0.f, 0.f, 1.f};
    memcpy(&transform_info.position[0], &origin, sizeof(transform_info.position));
    memcpy(&transform_info.rotation[0], &identity_quat, sizeof(transform_info.rotation));

    game_entity::entity_info einfo{};
    einfo.transform = &transform_info;
    game_entity::entity ent = game_entity::create(einfo);
    CHECK(ent.is_valid(), "entity created");

    camera_init_info ci{};
    ci.entity_id = ent.get_id();
    ci.type = graphics::camera::perspective;
    ci.up = {0.f, 1.f, 0.f};
    ci.field_of_view = 0.25f;
    ci.aspect_ratio = 1.f;
    ci.near_z = 0.1f;
    ci.far_z = 64.f;
    graphics::camera cam = create_camera(ci);
    camera_id cid = cam.get_id();
    CHECK(id::is_valid(cid), "camera created");

    rhi::ViewportDesc vp{};
    vp.topLeft  = math::v2{0.f, 0.f};
    vp.size     = math::v2{64.f, 64.f};
    vp.minDepth = 0.f;
    vp.maxDepth = 1.f;

    RenderView view = BuildRenderViewFromCameraId(cid, vp);

    // For an identity-transform camera, the view matrix should have zero
    // translation (column 3 xyz).
    const math::m4x4& viewM = view.GetViewMatrix();
    const bool zeroTranslation =
        viewM.columns[3][0] == 0.f &&
        viewM.columns[3][1] == 0.f &&
        viewM.columns[3][2] == 0.f;
    CHECK(zeroTranslation, "View translation zero at origin");

    // Viewport must be preserved.
    CHECK(view.GetViewport().size.x == 64.f, "Viewport size.x preserved");
    CHECK(view.GetViewport().size.y == 64.f, "Viewport size.y preserved");

    // ViewProjectionMatrix should be derived (not identity if projection != I).
    // For a perspective camera, projection diagonal != (1,1,1,1).
    const math::m4x4& projM = view.GetProjectionMatrix();
    const bool projectionNotIdentity = !(projM.columns[0][0] == 1.f &&
                                         projM.columns[1][1] == 1.f &&
                                         projM.columns[2][2] == 1.f &&
                                         projM.columns[3][3] == 1.f);
    CHECK(projectionNotIdentity, "Projection matrix populated (perspective)");

    remove_camera(cid);
    game_entity::remove(ent.get_id());
}

int main() {
    std::cout << "=================================" << std::endl;
    std::cout << "TestRenderViewBuilder" << std::endl;
    std::cout << "Task 1 Integration Test" << std::endl;
    std::cout << "=================================" << std::endl;

    // Case 1 doesn't need engine init — invalid-id early-out path.
    TestInvalidCameraIdReturnsDefault();

    // Case 2 needs full engine init. Use the three-step RHI path.
    bool liveRan = false;
    {
        rhi::DeviceDesc desc{};
        desc.platform = rhi::RHIPlatform::Metal;
        desc.enableDebug = false;
        if (!initialize_with_device(desc) || !bind_rhi_device_to_legacy()) {
            std::cerr << "[SKIP] Metal device unavailable — TestLiveCameraAtOrigin skipped"
                      << std::endl;
        } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            if (!initialize(graphics_platform::metal)) {
                std::cerr << "[SKIP] initialize(metal) failed (shaders missing) — "
                             "TestLiveCameraAtOrigin skipped"
                          << std::endl;
            } else {
                liveRan = true;
                TestLiveCameraAtOrigin();
                shutdown();
            }
#pragma GCC diagnostic pop
            shutdown_rhi();
        }
    }

    std::cout << "\n=================================" << std::endl;
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED"
                  << (liveRan ? "" : " (live camera test skipped)") << std::endl;
        return 0;
    }
    std::cout << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
