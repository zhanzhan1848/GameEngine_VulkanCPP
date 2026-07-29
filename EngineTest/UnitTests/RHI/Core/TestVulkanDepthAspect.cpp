/**
 * @file TestVulkanDepthAspect.cpp
 * @brief Phase 4b Tier 3 infrastructure test (T3.0).
 * @details Validates the VulkanTexture::GetAspectMask() fix end-to-end:
 *          D32_FLOAT textures must use VK_IMAGE_ASPECT_DEPTH_BIT (not the
 *          previously-hardcoded COLOR_BIT) for copy/blit/barrier paths.
 *
 * Flow: staging buffer (f32 pattern) → CopyBufferToTexture → CopyTextureToBuffer
 * → readback → verify round-trip. If the aspect mask is wrong, validation
 * fires and/or the readback comes back as zero bytes.
 *
 * No SPIR-V needed — pure copy path. No Metal reference needed — depth values
 * are exact f32 round-trip.
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#endif

#include <cstring>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

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
    ~DeviceFixture() {
        if (base) base->Shutdown();
    }
};

} // anonymous namespace

/// CopyBufferToTexture + CopyTextureToBuffer on a D32_FLOAT texture must
/// round-trip depth values exactly. Verifies GetAspectMask() returns
/// VK_IMAGE_ASPECT_DEPTH_BIT for D32_FLOAT (was hardcoded COLOR_BIT before
/// T3.0 — would have failed validation and/or produced zero readback).
TestResult TestDepthTextureCopyRoundTrip() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kW = 4, kH = 4;
    constexpr u32 kPixels = kW * kH;

    // Known f32 depth pattern: ascending 0.0, 0.1, 0.2, ... 1.5.
    std::vector<float> depthPattern(kPixels);
    for (u32 i = 0; i < kPixels; ++i) {
        depthPattern[i] = float(i) * 0.1f;  // 0.0 .. 1.5
    }
    const u64 bufBytes = u64(kPixels) * sizeof(float);

    // Staging buffer with the pattern.
    BufferDesc stagingDesc{};
    stagingDesc.size = bufBytes;
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "DepthStaging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    TEST_ASSERT(staging != handles::INVALID_RESOURCE, "CreateBuffer staging");
    TEST_ASSERT(fx.base->UpdateBufferData(staging, depthPattern.data(), bufBytes, 0),
                "UpdateBufferData staging");

    // D32_FLOAT depth texture with CopyDest | CopySource.
    TextureDesc tdesc{};
    tdesc.size = { kW, kH, 1 };
    tdesc.mipLevels = 1;
    tdesc.arraySize = 1;
    tdesc.format = DataFormat::D32_Float;
    tdesc.type = TextureType::Texture2D;
    tdesc.usage = TextureUsage::CopyDest | TextureUsage::CopySource | TextureUsage::DepthStencil;
    tdesc.memoryUsage = GPUMemoryUsage::Static;
    tdesc.name = "DepthRT";
    ResourceHandle depthTex = fx.base->CreateTexture(tdesc);
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "CreateTexture D32_FLOAT");

    // Sanity check: aspect mask must be DEPTH_BIT (T3.0a contract).
    VulkanTexture* vtex = fx.vk->GetTexture(depthTex);
    TEST_ASSERT(vtex, "GetTexture D32_FLOAT");
    VkImageAspectFlags aspect = vtex->GetAspectMask();
    std::cout << "[TestVulkanDepthAspect] D32_FLOAT aspect mask = 0x"
              << std::hex << aspect << std::dec
              << " (expect 0x2 = VK_IMAGE_ASPECT_DEPTH_BIT)" << std::endl;
    TEST_ASSERT(aspect == VK_IMAGE_ASPECT_DEPTH_BIT,
                "D32_FLOAT aspect must be VK_IMAGE_ASPECT_DEPTH_BIT");

    // Readback buffer.
    BufferDesc readbackDesc{};
    readbackDesc.size = bufBytes;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "DepthReadback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // Copy staging → depth texture → readback.
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer");
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd, "GetCommandBuffer");

    TEST_ASSERT(vcmd->Reset(), "Reset");
    TEST_ASSERT(vcmd->Begin(), "Begin");

    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};

    vcmd->CopyBufferToTexture(staging, depthTex, &region, 1);
    vcmd->CopyTextureToBuffer(depthTex, readback, &region, 1);

    TEST_ASSERT(vcmd->End(), "End");
    TEST_ASSERT(vcmd->Submit(0), "Submit");
    TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");

    // Verify round-trip.
    void* mapped = fx.base->MapBuffer(readback, 0, bufBytes);
    TEST_ASSERT(mapped, "MapBuffer readback");
    {
        const float* got = reinterpret_cast<const float*>(mapped);
        u32 mismatches = 0;
        float firstMismatchGot = 0.0f, firstMismatchExp = 0.0f;
        for (u32 i = 0; i < kPixels; ++i) {
            // Bit-exact compare — f32 round-trip through CopyBuffer/CopyTexture
            // must preserve the value (no format conversion for D32_FLOAT).
            std::uint32_t g, e;
            std::memcpy(&g, &got[i], 4);
            std::memcpy(&e, &depthPattern[i], 4);
            if (g != e) {
                if (mismatches == 0) {
                    firstMismatchGot = got[i];
                    firstMismatchExp = depthPattern[i];
                }
                ++mismatches;
            }
        }
        std::cout << "[TestVulkanDepthAspect] D32_FLOAT round-trip mismatches: "
                  << mismatches << "/" << kPixels << std::endl;
        if (mismatches > 0) {
            std::cout << "[TestVulkanDepthAspect] first mismatch: got="
                      << firstMismatchGot << " expected=" << firstMismatchExp
                      << std::endl;
        }
        TEST_ASSERT(mismatches == 0,
                    "All depth values must round-trip bit-exactly");
    }
    fx.base->UnmapBuffer(readback);

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyBuffer(staging);
    return TestResult::Passed;
}

/// Color texture (RGBA8) aspect must still return COLOR_BIT — guards against
/// over-broad fix that might accidentally apply depth aspect to color formats.
TestResult TestColorTextureAspectStillColor() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    TextureDesc tdesc{};
    tdesc.size = { 4, 4, 1 };
    tdesc.mipLevels = 1;
    tdesc.arraySize = 1;
    tdesc.format = DataFormat::RG8B8A8_UNorm;
    tdesc.type = TextureType::Texture2D;
    tdesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    tdesc.memoryUsage = GPUMemoryUsage::Static;
    tdesc.name = "ColorRT";
    ResourceHandle colorTex = fx.base->CreateTexture(tdesc);
    TEST_ASSERT(colorTex != handles::INVALID_RESOURCE, "CreateTexture RGBA8");

    VulkanTexture* vtex = fx.vk->GetTexture(colorTex);
    TEST_ASSERT(vtex, "GetTexture RGBA8");
    VkImageAspectFlags aspect = vtex->GetAspectMask();
    std::cout << "[TestVulkanDepthAspect] RGBA8 aspect mask = 0x"
              << std::hex << aspect << std::dec
              << " (expect 0x1 = VK_IMAGE_ASPECT_COLOR_BIT)" << std::endl;
    TEST_ASSERT(aspect == VK_IMAGE_ASPECT_COLOR_BIT,
                "RGBA8 aspect must remain COLOR_BIT (regression guard)");

    fx.base->DestroyTexture(colorTex);
    return TestResult::Passed;
}

void RegisterVulkanDepthAspectTests() {
    auto suite = std::make_shared<TestSuite>("VulkanDepthAspectTests");
    suite->AddTestCase(TestCase("DepthTextureCopyRoundTrip",   TestDepthTextureCopyRoundTrip));
    suite->AddTestCase(TestCase("ColorTextureAspectStillColor", TestColorTextureAspectStillColor));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanDepthAspectTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanDepthAspect] ENABLE_VULKAN not defined — test is a no-op build check." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
