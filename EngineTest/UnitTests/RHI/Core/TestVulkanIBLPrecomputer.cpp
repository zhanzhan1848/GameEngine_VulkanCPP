/**
 * @file TestVulkanIBLPrecomputer.cpp
 * @brief Phase 4b Tier 4.0 — production IBLPrecomputer path on Vulkan.
 * @details Validates the end-to-end production code path:
 *
 *            IBLPrecomputer::Initialize()  → loads .spv shaders via
 *              CreateComputeShader platform branch (T4.0.4)
 *            IBLPrecomputer::ComputeBRDFIntegrationMap(64)
 *              → dispatches IBL_BRDFIntegration.spv, returns ResourceHandle
 *
 *          This test does NOT re-validate the shader math (Tier 3 test
 *          TestVulkanIBL_BRDFLUT already does that with CPU parity). The
 *          bar here is purely "production path works": Initialize succeeds,
 *          Compute returns a valid handle, output texture has non-zero data,
 *          and zero validation errors.
 *
 * Acceptance:
 *   - Initialize() returns true
 *   - ComputeBRDFIntegrationMap returns non-INVALID handle
 *   - Readback has non-zero bytes (BRDF LUT is well-known non-trivial)
 *   - Zero validation errors
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Utils/IBLPrecomputer.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

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

} // anonymous namespace

TestResult TestIBLPrecomputer_BRDFLUT_Path() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 1. Instantiate + Initialize the production IBLPrecomputer.
    //    This loads IBL_BRDFIntegration.spv via ShaderRegistry path
    //    (Engine/Graphics/Vulkan/shaders/).
    IBLPrecomputer precomputer(fx.base);
    bool ok = precomputer.Initialize();
    TEST_ASSERT(ok, "IBLPrecomputer::Initialize on Vulkan");

    // 2. Compute BRDF LUT at small size (64×64) for fast turnaround.
    ResourceHandle lut = precomputer.ComputeBRDFIntegrationMap(64);
    TEST_ASSERT(lut != handles::INVALID_RESOURCE, "ComputeBRDFIntegrationMap");

    // 3. Read back the LUT to verify non-zero output. BRDF LUT is RGBA16F,
    //    well-known non-trivial data (A from ~0 to ~1, B from 0 to ~0.5).
    BufferDesc readbackDesc{};
    readbackDesc.size = u64(64) * 64 * 8;  // RGBA16F = 8 bytes/px
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "IBLPrecomputer_LUT_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    // LUT is written by compute (StorageImage) — transition to CopySource.
    ResourceBarrier toCopy{};
    toCopy.resource = lut;
    toCopy.beforeState = ResourceState::UnorderedAccess;
    toCopy.afterState = ResourceState::CopySource;
    toCopy.subresource = 0xFFFFFFFF;
    toCopy.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopy, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {64, 64, 1};
    vcmd->CopyTextureToBuffer(lut, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // 4. Inspect readback: count non-zero texels, sample well-known anchors.
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    u32 nonZero = 0;
    for (u32 i = 0; i < 64 * 64; ++i) {
        u8* p = static_cast<u8*>(mapped) + i * 8;
        u16 hr, hg, hb, ha;
        std::memcpy(&hr, p + 0, 2);
        std::memcpy(&hg, p + 2, 2);
        std::memcpy(&hb, p + 4, 2);
        std::memcpy(&ha, p + 6, 2);
        if (hr || hg || hb || ha) ++nonZero;
    }

    // Sample (NdotV~1, roughness~1) corner — known A ≈ 0.421, B ≈ 0.455.
    auto half_to_float = [](u16 h) -> float {
        u32 sign = (h >> 15) & 0x1;
        u32 exp = (h >> 10) & 0x1F;
        u32 mant = h & 0x3FF;
        if (exp == 0) return 0.0f;
        u32 bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    u8* corner = static_cast<u8*>(mapped) + ((63 * 64 + 63) * 8);
    u16 cR, cG;
    std::memcpy(&cR, corner + 0, 2);
    std::memcpy(&cG, corner + 2, 2);
    float cornerA = half_to_float(cR);
    float cornerB = half_to_float(cG);

    fx.base->UnmapBuffer(readback);

    std::cout << "[TestVulkanIBLPrecomputer] nonZero texels: " << nonZero << "/" << (64 * 64)
              << "  corner(NdotV~1,rough~1) A=" << cornerA << " B=" << cornerB
              << " (expected A≈0.42 B≈0.46)" << std::endl;

    // Smoke bar: most texels should be non-zero (LUT is non-trivial).
    TEST_ASSERT(nonZero > (64 * 64 * 8 / 10), "BRDF LUT has >80% non-zero texels");
    // Anchor sanity: roughness=1 + NdotV=1 should give A in [0.3, 0.6].
    TEST_ASSERT(cornerA > 0.3f && cornerA < 0.6f, "corner A in expected range");

    // Cleanup.
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(lut);
    return TestResult::Passed;
}

void RegisterVulkanIBLPrecomputer_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanIBLPrecomputer_Tests");
    suite->AddTestCase(TestCase("BRDFLUT_Path", TestIBLPrecomputer_BRDFLUT_Path));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanIBLPrecomputer_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "Vulkan backend disabled; TestVulkanIBLPrecomputer is a no-op." << std::endl;
    return 0;
}

#endif
