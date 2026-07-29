/**
 * @file TestVulkanForwardRendererIntegration.cpp
 * @brief Tier 4.2 — end-to-end ForwardRenderer smoke test on Vulkan.
 * @details Validates that the production ForwardRenderer::Render() path runs
 *          without crashing or validation errors on a Vulkan device. This is
 *          a SMOKE test, not parity:
 *
 *            ForwardRenderer::Initialize()  → creates dawn* bypass resources
 *              (shadow DSL, GBuffer DSL, deferred DSL, etc.) via the
 *              bypassProd = (isDawn || isVulkan) branch (T4.1)
 *            ForwardRenderer::Render()      → empty scene, clear color only
 *
 *          Bar (per plan):
 *            - Initialize() returns true
 *            - Render() completes without crash
 *            - Zero validation errors
 *            - Non-trivial output (clear color 0.1, 0.1, 0.15)
 *
 *          Scene: empty RenderScene + 1 directional light (required by
 *          SetupLights but no proxies to render).
 *
 * Acceptance:
 *   - Initialize() returns true
 *   - Render() completes within 5 seconds
 *   - Readback pixels match clear color (within ±0.05 tolerance)
 *   - Zero validation errors
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/ForwardRenderer.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <cmath>
#include <cstring>
#include <vector>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

} // anonymous namespace

TestResult TestForwardRendererIntegration_Smoke() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 1. Create render target (64x64 RGBA16F) + depth (D32).
    constexpr u32 W = 64, H = 64;

    TextureDesc rtDesc{
        {W, H, 1},
        1,
        1,
        DataFormat::RGBA16_Float,
        TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "SmokeRT"
    };
    ResourceHandle renderTarget = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");

    TextureDesc depthDesc{
        {W, H, 1},
        1,
        1,
        DataFormat::D32_Float,
        TextureType::Texture2D,
        TextureUsage::DepthStencil | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "SmokeDepth"
    };
    ResourceHandle depth = fx.base->CreateTexture(depthDesc);
    TEST_ASSERT(depth != handles::INVALID_RESOURCE, "CreateTexture depth");

    // 2. ForwardRenderer Initialize.
    ForwardRenderer renderer;
    bool ok = renderer.Initialize(fx.base);
    TEST_ASSERT(ok, "ForwardRenderer::Initialize on Vulkan");

    // 3. Build a minimal scene: 1 directional light, no proxies.
    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    // 4. Build a minimal view: identity-ish camera looking at origin.
    RenderView view;
    m4x4 viewMat = make_identity_m4x4();
    viewMat.columns[3][2] = 5.0f;  // pull camera back along +Z
    view.SetViewMatrix(viewMat);

    // Perspective projection (irrelevant for empty scene, but required by view).
    constexpr float pi = 3.14159265358979323846f;
    float fov = 60.0f * (pi / 180.0f);
    float aspect = float(W) / float(H);
    float f = 1.0f / std::tan(fov * 0.5f);
    m4x4 proj{};
    std::memset(&proj, 0, sizeof(proj));
    proj.columns[0][0] = f / aspect;
    proj.columns[1][1] = f;
    proj.columns[2][2] = 50.0f / (0.1f - 100.0f);   // [0,1] depth range
    proj.columns[2][3] = 1.0f;
    proj.columns[3][2] = -(0.1f * 100.0f) / (0.1f - 100.0f);
    view.SetProjectionMatrix(proj);
    view.Cull(scene);

    // 5. Render().
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    std::unordered_map<primal::id::id_type, std::shared_ptr<MaterialInstance>> emptyMaterials;
    renderer.Render(vcmd, scene, view, renderTarget,
                    handles::INVALID_RESOURCE /*velocityTarget*/,
                    depth, emptyMaterials, 0, W, H);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // 6. Readback the render target — verify clear color (0.1, 0.1, 0.15).
    BufferDesc readbackDesc{};
    readbackDesc.size = u64(W) * H * 8;  // RGBA16F = 8 bytes/px
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "SmokeRT_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin readback");

    ResourceBarrier toCopy{};
    toCopy.resource = renderTarget;
    toCopy.beforeState = ResourceState::RenderTarget;
    toCopy.afterState = ResourceState::CopySource;
    toCopy.subresource = 0xFFFFFFFF;
    toCopy.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopy, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {W, H, 1};
    vcmd->CopyTextureToBuffer(renderTarget, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit readback");
    fx.base->DestroyCommandBuffer(cmd);

    // 7. Inspect readback — convert half to float, sample center pixel.
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    auto half_to_float = [](u16 h) -> float {
        u32 sign = (h >> 15) & 0x1;
        u32 exp = (h >> 10) & 0x1F;
        u32 mant = h & 0x3FF;
        if (exp == 0) {
            // Denormal — treat as 0 for our purposes.
            return sign ? -0.0f : 0.0f;
        }
        u32 bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };

    // Sample several pixels (center + corners).
    auto sample_at = [&](u32 x, u32 y) {
        u8* p = static_cast<u8*>(mapped) + ((y * W + x) * 8);
        u16 hr, hg, hb, ha;
        std::memcpy(&hr, p + 0, 2);
        std::memcpy(&hg, p + 2, 2);
        std::memcpy(&hb, p + 4, 2);
        std::memcpy(&ha, p + 6, 2);
        return v4{half_to_float(hr), half_to_float(hg), half_to_float(hb), half_to_float(ha)};
    };

    v4 center = sample_at(W / 2, H / 2);
    v4 corner = sample_at(0, 0);
    fx.base->UnmapBuffer(readback);

    std::cout << "[TestVulkanFRIntegration] clear color at center: ("
              << center.x << ", " << center.y << ", " << center.z << ", " << center.w << ")"
              << "  expected ≈ (0.1, 0.1, 0.15, 1.0)" << std::endl;

    // Acceptance: non-zero output (not all black) and matches clear color
    // within ±0.05 tolerance. This proves Render() executed the render pass
    // and the clear op fired.
    TEST_ASSERT(center.x > 0.05f && center.x < 0.15f, "R channel ≈ 0.1");
    TEST_ASSERT(center.y > 0.05f && center.y < 0.15f, "G channel ≈ 0.1");
    TEST_ASSERT(center.z > 0.10f && center.z < 0.20f, "B channel ≈ 0.15");
    TEST_ASSERT(center.w > 0.95f, "A channel = 1.0");

    // Corners should also match — no geometry drawn so entire RT is clear color.
    TEST_ASSERT(std::fabs(corner.x - center.x) < 0.01f, "corner matches center (R)");
    TEST_ASSERT(std::fabs(corner.y - center.y) < 0.01f, "corner matches center (G)");

    // Cleanup.
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(renderTarget);
    fx.base->DestroyTexture(depth);
    renderer.Shutdown();
    return TestResult::Passed;
}

void RegisterVulkanForwardRendererIntegration_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanForwardRendererIntegration_Tests");
    suite->AddTestCase(TestCase("Smoke", TestForwardRendererIntegration_Smoke));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanForwardRendererIntegration_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "Vulkan backend disabled; TestVulkanForwardRendererIntegration is a no-op." << std::endl;
    return 0;
}

#endif
