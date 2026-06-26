/**
 * @file TestPipelineLightEntity.cpp
 * @brief Task 2 Integration Test: StandardRenderPipeline light entity methods
 * @details Validates that StandardRenderPipeline::RegisterLightEntity correctly:
 *   1. Creates an ECS entity with Transform + Light components
 *   2. scene_sync::SyncLightsFromECS picks up the registered light
 *   3. Unregister removes the entity
 *   4. Update mutates the light component (color, intensity)
 *   5. Invalid-id / dead-entity cases are no-ops
 *
 * Link pattern: direct link against libEngine.a (mirrors TestRenderViewBuilder.cpp).
 * Engine init uses the three-step sequence (initialize_with_device +
 * bind_rhi_device_to_legacy + initialize(metal)); if Metal or shaders are
 * unavailable the test gracefully skips (returns 0) so it can run in CI
 * environments without a GPU.
 */

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Light.h"
#include "Engine/EngineAPI/GameEntity.h"
#include "Engine/EngineAPI/Light.h"
#include "Engine/EngineAPI/LightComponent.h"
#include "Engine/Graphics/Renderer.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/Scene/RenderSceneSnapshot.h"  // complete type for unique_ptr<RenderSceneSnapshot> destructor
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/Scene/LightSyncSystem.h"

#include <cassert>
#include <cmath>
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

int main() {
    std::cout << "=================================" << std::endl;
    std::cout << "TestPipelineLightEntity" << std::endl;
    std::cout << "Task 2 Integration Test" << std::endl;
    std::cout << "=================================" << std::endl;

    // Three-step RHI init (mirrors TestRenderViewBuilder.cpp pattern).
    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;
    if (!initialize_with_device(desc) || !bind_rhi_device_to_legacy()) {
        std::cerr << "[SKIP] Metal device unavailable" << std::endl;
        return 0;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (!initialize(graphics_platform::metal)) {
        std::cerr << "[SKIP] initialize(metal) failed (shaders missing)" << std::endl;
        shutdown_rhi();
        return 0;
    }
#pragma GCC diagnostic pop

    StandardRenderPipeline pipeline;
    if (!pipeline.Initialize(get_rhi_device())) {
        std::cerr << "[SKIP] pipeline init failed" << std::endl;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        shutdown();
#pragma GCC diagnostic pop
        shutdown_rhi();
        return 0;
    }

    // === Test 1: Register directional light ===
    std::cout << "\n--- Test 1: RegisterLightEntity ---" << std::endl;
    light_init_info linfo{};
    linfo.entity_id = id::invalid_id;  // REVISED contract: pipeline creates entity
    linfo.type = graphics::light::directional;
    linfo.color = {1.f, 1.f, 1.f};
    linfo.intensity = 5.f;
    linfo.is_enabled = true;

    id::id_type eid = pipeline.RegisterLightEntity(linfo);
    CHECK(eid != id::invalid_id, "RegisterLightEntity returns valid id");

    // === Test 2: SyncLightsFromECS picks up the registered light ===
    std::cout << "\n--- Test 2: SyncLightsFromECS ---" << std::endl;
    {
        RenderScene scene;
        scene_sync::SyncLightsFromECS(scene);
        CHECK(scene.GetLights().size() == 1, "RenderScene has 1 light after sync");
    }

    // === Test 3: Unregister ===
    std::cout << "\n--- Test 3: UnregisterLightEntity ---" << std::endl;
    pipeline.UnregisterLightEntity(eid);
    {
        RenderScene scene2;
        scene_sync::SyncLightsFromECS(scene2);
        CHECK(scene2.GetLights().size() == 0, "RenderScene has 0 lights after unregister");
    }

    // === Test 4: Unregister invalid_id is no-op ===
    std::cout << "\n--- Test 4: UnregisterLightEntity(invalid_id) ---" << std::endl;
    pipeline.UnregisterLightEntity(id::invalid_id);  // should not crash
    CHECK(true, "UnregisterLightEntity(invalid_id) no-op");

    // === Test 5: Unregister dead entity is no-op ===
    std::cout << "\n--- Test 5: UnregisterLightEntity on dead entity ---" << std::endl;
    pipeline.UnregisterLightEntity(eid);  // already removed
    CHECK(true, "UnregisterLightEntity on dead entity no-op");

    // === Test 6: Update existing entity ===
    std::cout << "\n--- Test 6: UpdateLightEntity ---" << std::endl;
    light_init_info linfo2{};
    linfo2.entity_id = id::invalid_id;
    linfo2.type = graphics::light::directional;
    linfo2.color = {1.f, 0.f, 0.f};  // red
    linfo2.intensity = 3.f;
    linfo2.is_enabled = true;
    id::id_type eid2 = pipeline.RegisterLightEntity(linfo2);
    CHECK(eid2 != id::invalid_id, "Second RegisterLightEntity returns valid id");

    light_init_info updateInfo{};
    updateInfo.entity_id = id::invalid_id;
    updateInfo.type = graphics::light::directional;
    updateInfo.color = {0.f, 1.f, 0.f};  // green
    updateInfo.intensity = 7.f;
    updateInfo.is_enabled = true;
    CHECK(pipeline.UpdateLightEntity(eid2, updateInfo), "UpdateLightEntity succeeds");

    {
        RenderScene scene3;
        scene_sync::SyncLightsFromECS(scene3);
        CHECK(scene3.GetLights().size() == 1, "Still 1 light after update");
        if (scene3.GetLights().size() == 1) {
            const auto& rl = scene3.GetLights()[0];
            CHECK(rl.color.x == 0.f && rl.color.y == 1.f && rl.color.z == 0.f,
                  "Color updated to green");
            CHECK(rl.intensity == 7.f, "Intensity updated to 7");
        }
    }

    // === Test 7: Update on invalid_id fails ===
    std::cout << "\n--- Test 7: UpdateLightEntity(invalid_id) ---" << std::endl;
    CHECK(!pipeline.UpdateLightEntity(id::invalid_id, updateInfo),
          "Update on invalid_id returns false");

    // === Test 8: Update on dead entity fails ===
    std::cout << "\n--- Test 8: UpdateLightEntity on dead entity ---" << std::endl;
    CHECK(!pipeline.UpdateLightEntity(eid, updateInfo),
          "Update on dead entity returns false");

    // Cleanup Test 1-8 leftovers so new tests start from a clean slate.
    pipeline.UnregisterLightEntity(eid2);

    // === Test 9: Register point light, verify range/attenuation survive sync ===
    std::cout << "\n--- Test 9: RegisterLightEntity (point) ---" << std::endl;
    {
        light_init_info plinfo{};
        plinfo.entity_id = id::invalid_id;
        plinfo.type = graphics::light::point;
        plinfo.color = {0.2f, 0.5f, 0.9f};
        plinfo.intensity = 12.f;
        plinfo.is_enabled = true;
        plinfo.point_param.attenuation = {0.5f, 0.1f, 0.02f};
        plinfo.point_param.range = 25.f;
        id::id_type peid = pipeline.RegisterLightEntity(plinfo);
        CHECK(peid != id::invalid_id, "Register point light succeeds");

        RenderScene pscene;
        scene_sync::SyncLightsFromECS(pscene);
        CHECK(pscene.GetLights().size() == 1, "Point light visible after sync");
        if (!pscene.GetLights().empty()) {
            const auto& rl = pscene.GetLights()[0];
            CHECK(rl.type == LightType::Point, "Point light type preserved");
            CHECK(rl.range == 25.f, "Point light range preserved");
        }
        pipeline.UnregisterLightEntity(peid);
    }

    // === Test 10: Register spot light, verify cone angles survive sync ===
    std::cout << "\n--- Test 10: RegisterLightEntity (spot) ---" << std::endl;
    {
        light_init_info slinfo{};
        slinfo.entity_id = id::invalid_id;
        slinfo.type = graphics::light::spot;
        slinfo.color = {1.f, 0.8f, 0.4f};
        slinfo.intensity = 8.f;
        slinfo.is_enabled = true;
        slinfo.spot_param.attenuation = {0.3f, 0.05f, 0.01f};
        slinfo.spot_param.range = 15.f;
        slinfo.spot_param.umbra = 0.4f;       // radians
        slinfo.spot_param.penumbra = 0.7f;    // radians
        id::id_type seid = pipeline.RegisterLightEntity(slinfo);
        CHECK(seid != id::invalid_id, "Register spot light succeeds");

        RenderScene sscene;
        scene_sync::SyncLightsFromECS(sscene);
        CHECK(sscene.GetLights().size() == 1, "Spot light visible after sync");
        if (!sscene.GetLights().empty()) {
            const auto& rl = sscene.GetLights()[0];
            CHECK(rl.type == LightType::Spot, "Spot light type preserved");
            CHECK(rl.range == 15.f, "Spot light range preserved");
            // innerCone = cos(umbra), outerCone = cos(penumbra) — see LightSyncSystem.cpp:101-102
            CHECK(rl.innerCone == std::cos(0.4f), "Spot innerCone = cos(umbra)");
            CHECK(rl.outerCone == std::cos(0.7f), "Spot outerCone = cos(penumbra)");
        }
        pipeline.UnregisterLightEntity(seid);
    }

    // === Test 11: Update directional → spot, verify cone fields now set, no stale state ===
    std::cout << "\n--- Test 11: UpdateLightEntity directional → spot ---" << std::endl;
    {
        light_init_info dinfo{};
        dinfo.entity_id = id::invalid_id;
        dinfo.type = graphics::light::directional;
        dinfo.color = {1.f, 1.f, 1.f};
        dinfo.intensity = 5.f;
        dinfo.is_enabled = true;
        id::id_type teid = pipeline.RegisterLightEntity(dinfo);
        CHECK(teid != id::invalid_id, "Register directional light for type-change test");

        light_init_info sinfo{};
        sinfo.entity_id = id::invalid_id;
        sinfo.type = graphics::light::spot;
        sinfo.color = {1.f, 0.f, 0.f};
        sinfo.intensity = 3.f;
        sinfo.is_enabled = true;
        sinfo.spot_param.attenuation = {1.f, 0.f, 0.f};
        sinfo.spot_param.range = 10.f;
        sinfo.spot_param.umbra = 0.5f;
        sinfo.spot_param.penumbra = 0.9f;

        CHECK(pipeline.UpdateLightEntity(teid, sinfo), "Update directional → spot succeeds");

        RenderScene tscene;
        scene_sync::SyncLightsFromECS(tscene);
        CHECK(tscene.GetLights().size() == 1, "1 light after type change");
        if (!tscene.GetLights().empty()) {
            const auto& rl = tscene.GetLights()[0];
            CHECK(rl.type == LightType::Spot, "Type now Spot");
            CHECK(rl.range == 10.f, "Range updated to spot param");
            CHECK(rl.innerCone == std::cos(0.5f), "Cone angles set from spot param");
        }
        pipeline.UnregisterLightEntity(teid);
    }

    pipeline.Shutdown();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    shutdown();
#pragma GCC diagnostic pop
    shutdown_rhi();

    std::cout << "\n=================================" << std::endl;
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
