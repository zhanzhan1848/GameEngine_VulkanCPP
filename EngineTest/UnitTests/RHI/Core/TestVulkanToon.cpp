/**
 * @file TestVulkanToon.cpp
 * @brief Toon post-process on Vulkan — quantization + depth Sobel edges.
 * @details Drives the real renderpass::AddToonPass through a RenderGraph.
 *
 *          Input: RGBA8 color gradient (R = x/W) + D32 depth with a hard
 *          vertical split (left 0.2 / right 0.8) + a dummy normal texture.
 *          colorLevels = 4, edgeThreshold = 0.1.
 *
 *          Expected (closed-form, matches PostProcess/Toon.comp):
 *            - Away from the split column: out = floor(c * 4) / 4
 *            - At the split column (and its 1-px neighbors): depth-delta sum
 *              0.6 > 0.1 → edgeFactor 0 → black
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToonPass.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::graphics;
using namespace primal::graphics::rendergraph;
using namespace primal::math;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64;
constexpr u32 kH = 64;
constexpr float kLevels = 4.0f;
constexpr float kEdgeThreshold = 0.1f;

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

ResourceHandle CreateAndUploadTexture(DeviceFixture& fx,
                                      const TextureDesc& desc,
                                      const std::vector<u8>& pixels,
                                      const char* name) {
    TextureDesc d = desc;
    d.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
    d.memoryUsage = GPUMemoryUsage::Static;
    d.name = name;
    ResourceHandle tex = fx.base->CreateTexture(d);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc stagingDesc{};
    stagingDesc.size = pixels.size();
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "Staging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE ||
        !fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0)) {
        fx.base->DestroyTexture(tex);
        if (staging != handles::INVALID_RESOURCE) fx.base->DestroyBuffer(staging);
        return handles::INVALID_RESOURCE;
    }

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { d.size.x, d.size.y, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);
    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = ResourceState::ShaderResource;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

} // anonymous namespace

TestResult TestToon_QuantizeAndEdges() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // Color: R gradient x/W*255, RGBA8.
    std::vector<u8> colorPixels(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            u8* p = colorPixels.data() + (size_t(y) * kW + x) * 4;
            p[0] = static_cast<u8>(255u * x / (kW - 1));
            p[1] = 128; p[2] = 64; p[3] = 255;
        }
    }

    // Depth: hard vertical split at x == kW/2 (left 0.2 / right 0.8), D32 float.
    std::vector<u8> depthPixels(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float d = (x < kW / 2) ? 0.2f : 0.8f;
            std::memcpy(depthPixels.data() + (size_t(y) * kW + x) * 4, &d, 4);
        }
    }

    TextureDesc colorDesc{};
    colorDesc.size = { kW, kH, 1 };
    colorDesc.mipLevels = 1;
    colorDesc.arraySize = 1;
    colorDesc.format = DataFormat::RGBA8_UNorm;
    colorDesc.type = TextureType::Texture2D;
    ResourceHandle colorTex = CreateAndUploadTexture(fx, colorDesc, colorPixels, "Toon_Color");
    TEST_ASSERT(colorTex != handles::INVALID_RESOURCE, "CreateTexture colorTex");

    TextureDesc depthDesc = colorDesc;
    depthDesc.format = DataFormat::D32_Float;
    ResourceHandle depthTex = CreateAndUploadTexture(fx, depthDesc, depthPixels, "Toon_Depth");
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "CreateTexture depthTex");

    // Normal is declared but unused by the shader — bind color as stand-in.
    ResourceHandle normalTex = colorTex;

    BufferDesc readbackDesc{};
    readbackDesc.size = kW * kH * 4;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.usage = GPUMemoryUsage::Readback;
    readbackDesc.name = "Toon_Readback";
    ResourceHandle readbackBuf = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readbackBuf != handles::INVALID_RESOURCE, "CreateBuffer readback");

    {
        RenderGraph graph(*fx.base);
        RGResourceHandle colorRG = graph.ImportResource("Toon_Test_Color", colorTex,
                                                        ResourceState::ShaderResource);
        RGResourceHandle depthRG = graph.ImportResource("Toon_Test_Depth", depthTex,
                                                        ResourceState::ShaderResource);
        RGResourceHandle normalRG = graph.ImportResource("Toon_Test_Normal", normalTex,
                                                         ResourceState::ShaderResource);

        renderpass::ToonParams params;
        params.colorLevels = kLevels;
        params.edgeThreshold = kEdgeThreshold;
        auto toonOut = renderpass::AddToonPass(graph, colorRG, depthRG, normalRG,
                                               params, 0, kW, kH);
        graph.MarkAsOutput(toonOut.toonOutput);

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin Toon dispatch");

        graph.Compile();
        graph.Execute(vcmd);

        auto* outRes = graph.GetResource(toonOut.toonOutput);
        TEST_ASSERT(outRes != nullptr, "GetResource Toon_Output");
        ResourceHandle outTex = outRes->GetPhysicalHandle();
        TEST_ASSERT(outTex != handles::INVALID_RESOURCE, "Toon_Output physical handle");

        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = { kW, kH, 1 };
        vcmd->CopyTextureToBuffer(outTex, readbackBuf, &region, 1);

        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                    "Submit Toon + readback");
        fx.base->DestroyCommandBuffer(cmd);
    }

    {
        void* mapped = fx.base->MapBuffer(readbackBuf, 0, 0);
        TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
        const auto* px = static_cast<const u8*>(mapped);

        u32 badQuant = 0, badEdge = 0, edgePixels = 0;
        const u32 split = kW / 2;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const u8* got = px + (size_t(y) * kW + x) * 4;

                // Depth-delta sum > threshold within ±1 px of the split → edge.
                float d = (x < split) ? 0.2f : 0.8f;
                bool isEdge = false;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = int(x) + dx;
                    float nd = (nx < int(split)) ? 0.2f : 0.8f;
                    if (nx < 0 || nx >= int(kW)) nd = d;
                    if (std::fabs(d - nd) > kEdgeThreshold / 4.0f) { isEdge = true; break; }
                }
                // The shader sums |d - 4 neighbors| and compares > threshold;
                // a single differing neighbor (0.6 diff) already exceeds it.
                if (isEdge) {
                    ++edgePixels;
                    if (got[0] != 0 || got[1] != 0 || got[2] != 0) ++badEdge;
                    continue;
                }

                // Quantization: floor(c * 4) / 4 per channel, in 8-bit space.
                float r = float(255u * x / (kW - 1)) / 255.0f;
                float g = 128.0f / 255.0f;
                float b = 64.0f / 255.0f;
                u8 expR = u8(std::floor(r * kLevels) / kLevels * 255.0f + 0.5f);
                u8 expG = u8(std::floor(g * kLevels) / kLevels * 255.0f + 0.5f);
                u8 expB = u8(std::floor(b * kLevels) / kLevels * 255.0f + 0.5f);
                if (std::abs(int(got[0]) - int(expR)) > 1 ||
                    std::abs(int(got[1]) - int(expG)) > 1 ||
                    std::abs(int(got[2]) - int(expB)) > 1) {
                    ++badQuant;
                    if (badQuant <= 6) {
                        std::cout << "  [quant mismatch] x=" << x << " y=" << y
                                  << " got=(" << int(got[0]) << "," << int(got[1]) << ","
                                  << int(got[2]) << ")"
                                  << " exp=(" << int(expR) << "," << int(expG) << ","
                                  << int(expB) << ")" << std::endl;
                    }
                }
            }
        }
        fx.base->UnmapBuffer(readbackBuf);
        std::cout << "[TestVulkanToon] edgePixels=" << edgePixels
                  << " badEdge=" << badEdge << " badQuant=" << badQuant << std::endl;
        TEST_ASSERT(edgePixels >= 2 * kH, "Split column detected as edges (2 columns)");
        TEST_ASSERT(badEdge == 0, "Edge pixels are black");
        TEST_ASSERT(badQuant == 0, "Non-edge pixels match quantization reference");
    }

    fx.base->DestroyBuffer(readbackBuf);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyTexture(colorTex);
    return TestResult::Passed;
}

void RegisterVulkanToonTests() {
    auto suite = std::make_shared<TestSuite>("VulkanToonTests");
    suite->AddTestCase(TestCase("QuantizeAndEdges", TestToon_QuantizeAndEdges));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanToonTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanToon] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
