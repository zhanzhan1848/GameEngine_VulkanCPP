/**
 * @file TestVulkanTAA.cpp
 * @brief TAA pass on Vulkan — dispatch, history chain, and reprojection math.
 * @details Drives the real PostProcess::AddTAAPass through a RenderGraph (the
 *          same path StandardRenderPipeline uses), two frames in sequence:
 *
 *          Frame 0 (first-ever): input = vertical grey gradient, velocity = (0, 2/H).
 *          History is invalid → alpha = 1.0 → output must equal the input
 *          exactly (passthrough), and the gradient lands in history slot 0.
 *
 *          Frame 1: same gradient input. Velocity (0, 2/H) shifts historyUV by
 *          exactly one texel in Y (2/H NDC × 0.5 × H = 1 texel — lands on texel
 *          centers, so the shader's manual bilinear is an exact fetch). With a
 *          monotonic gradient the 3×3 neighborhood AABB always contains the
 *          shifted history value (it equals the AABB max), so variance clipping
 *          is a no-op. Expected: out(y) = 0.9·g(y+1) + 0.1·g(y).
 *
 *          This validates the SPIR-V shader, the 5-binding descriptor set, the
 *          params UBO (invHistoryValid), triple-buffered history write via
 *          BlitTexture, and the read-slot selection — the pieces the old
 *          "TAA resolve not yet functional on Vulkan" comment was gating on.
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"

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
using namespace primal::math;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64;
constexpr u32 kH = 64;

// ============================= Half-float helpers =============================
u16 float_to_half(float f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    u32 sign = (bits >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    u32 mant = bits & 0x7FFFFF;
    if (exp <= 0) {
        if (exp < -10) return static_cast<u16>(sign);
        mant |= 0x800000;
        u32 shift = static_cast<u32>(14 - exp);
        return static_cast<u16>(sign | (mant >> shift));
    } else if (exp == 0xFF - (127 - 15)) {
        return static_cast<u16>(sign | 0x7C00 | (mant >> 13));
    } else if (exp > 31) {
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

float half_to_float(u16 h) {
    u32 sign = (h >> 15) & 0x1;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            // Subnormal half → normalized float
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

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

// Create a 2D texture, upload pixels via staging, transition to ShaderResource.
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

TestResult TestTAA_TwoFrameAccumulation() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // Gradient g(y) = y/(H-1) in RGB, A=1, RGBA16F.
    auto g = [](u32 y) { return static_cast<float>(y) / static_cast<float>(kH - 1); };

    std::vector<u8> gradPixels(size_t(kW) * kH * 8);
    for (u32 y = 0; y < kH; ++y) {
        u16 hr = float_to_half(g(y));
        for (u32 x = 0; x < kW; ++x) {
            u8* p = gradPixels.data() + (size_t(y) * kW + x) * 8;
            std::memcpy(p + 0, &hr, 2);   // R
            std::memcpy(p + 2, &hr, 2);   // G
            std::memcpy(p + 4, &hr, 2);   // B
            u16 ha = float_to_half(1.0f);
            std::memcpy(p + 6, &ha, 2);   // A
        }
    }

    // Velocity (0, 2/H) NDC — 2/64 = 0.03125 is exact in fp16. historyUV shift
    // in Y = (2/H)·0.5·H = exactly 1 texel, landing on texel centers.
    const float kVelY = 2.0f / static_cast<float>(kH);
    std::vector<u8> velPixels(size_t(kW) * kH * 4);
    {
        u16 hx = float_to_half(0.0f), hy = float_to_half(kVelY);
        for (u32 i = 0; i < kW * kH; ++i) {
            std::memcpy(velPixels.data() + size_t(i) * 4 + 0, &hx, 2);
            std::memcpy(velPixels.data() + size_t(i) * 4 + 2, &hy, 2);
        }
    }

    TextureDesc inDesc{};
    inDesc.size = { kW, kH, 1 };
    inDesc.mipLevels = 1;
    inDesc.arraySize = 1;
    inDesc.format = DataFormat::RGBA16_Float;
    inDesc.type = TextureType::Texture2D;
    ResourceHandle inputTex = CreateAndUploadTexture(fx, inDesc, gradPixels, "TAA_Test_Input");
    TEST_ASSERT(inputTex != handles::INVALID_RESOURCE, "CreateTexture inputTex");

    TextureDesc velDesc = inDesc;
    velDesc.format = DataFormat::RG16_Float;
    ResourceHandle velTex = CreateAndUploadTexture(fx, velDesc, velPixels, "TAA_Test_Vel");
    TEST_ASSERT(velTex != handles::INVALID_RESOURCE, "CreateTexture velTex");

    // Readback buffer (RGBA16F = 8 bytes/pixel), reused across frames.
    BufferDesc readbackDesc{};
    readbackDesc.size = kW * kH * 8;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.usage = GPUMemoryUsage::Readback;
    readbackDesc.name = "TAA_Readback";
    ResourceHandle readbackBuf = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readbackBuf != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // Fresh history — the pass keeps process-global statics, so a previous
    // test binary run in the same process would leave stale slots. Not the
    // case here, but ResetTAAHistory makes the precondition explicit.
    PostProcess::ResetTAAHistory();

    // Runs one TAA frame through a real RenderGraph and copies the result
    // into readbackBuf. Must complete before `graph` leaves scope — the
    // RenderGraph destructor destroys the pooled TAA_Output texture.
    auto runFrame = [&](u32 frameIndex) {
        RenderGraph graph(*fx.base);
        // Imported textures were transitioned to ShaderResource at upload —
        // declare it so the RG generates correct from-initial-state barriers.
        RGResourceHandle inRG = graph.ImportResource("TAA_Test_Input", inputTex,
                                                     ResourceState::ShaderResource);
        RGResourceHandle velRG = graph.ImportResource("TAA_Test_Vel", velTex,
                                                      ResourceState::ShaderResource);
        auto taaOut = PostProcess::AddTAAPass(graph, inRG, velRG, kW, kH, frameIndex);
        RGResourceHandle outRG = taaOut.output;
        // In the production pipeline Bloom/ToneMapping consume TAA_Output, which
        // keeps the pass alive. This minimal graph has no downstream pass —
        // without MarkAsOutput the RG culls the TAA pass and never allocates
        // the output texture (physical handle stays INVALID).
        graph.MarkAsOutput(outRG);

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin TAA frame");

        graph.Compile();
        graph.Execute(vcmd);

        auto* outRes = graph.GetResource(outRG);
        TEST_ASSERT(outRes != nullptr, "GetResource TAA_Output");
        ResourceHandle outTex = outRes->GetPhysicalHandle();
        TEST_ASSERT(outTex != handles::INVALID_RESOURCE, "TAA_Output physical handle");

        // After the pass the output's last op was the history BlitTexture —
        // its tracked layout is already TRANSFER_SRC, which is exactly what
        // CopyTextureToBuffer needs (it transitions from the tracked layout).
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // tight
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = { kW, kH, 1 };
        vcmd->CopyTextureToBuffer(outTex, readbackBuf, &region, 1);

        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                    "Submit TAA frame + readback");
        fx.base->DestroyCommandBuffer(cmd);
    };

    // ---------------- Frame 0: first-ever → alpha = 1.0 passthrough ----------------
    runFrame(0);

    {
        void* mapped = fx.base->MapBuffer(readbackBuf, 0, 0);
        TEST_ASSERT(mapped != nullptr, "MapBuffer frame 0");
        const auto* px = static_cast<const u8*>(mapped);
        float maxDiff = 0.0f;
        u32 badPixels = 0;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                // Sample the G channel of a few columns (rows are uniform).
                if ((x & 7) != 0) continue;
                u16 h;
                std::memcpy(&h, px + (size_t(y) * kW + x) * 8 + 2, 2);
                float got = half_to_float(h);
                float diff = std::fabs(got - g(y));
                if (diff > maxDiff) maxDiff = diff;
                if (diff > 0.002f) ++badPixels;
            }
        }
        fx.base->UnmapBuffer(readbackBuf);
        std::cout << "[TestVulkanTAA] Frame 0 passthrough: maxDiff=" << maxDiff
                  << " badPixels=" << badPixels << std::endl;
        TEST_ASSERT(badPixels == 0, "Frame 0 output == input (alpha=1 passthrough)");
    }

    // ---------------- Frame 1: history blend with 1-texel reproject ----------------
    // out(y) = 0.9·g(clamp(y+1)) + 0.1·g(y) — variance clip is a no-op for the
    // monotonic gradient (history value == neighborhood AABB max).
    runFrame(1);

    {
        void* mapped = fx.base->MapBuffer(readbackBuf, 0, 0);
        TEST_ASSERT(mapped != nullptr, "MapBuffer frame 1");
        const auto* px = static_cast<const u8*>(mapped);
        float maxDiff = 0.0f;
        u32 badPixels = 0;
        for (u32 y = 0; y < kH; ++y) {
            u32 hy = (y + 1 < kH) ? y + 1 : kH - 1;
            float expected = 0.9f * g(hy) + 0.1f * g(y);
            for (u32 x = 0; x < kW; ++x) {
                if ((x & 7) != 0) continue;
                u16 h;
                std::memcpy(&h, px + (size_t(y) * kW + x) * 8 + 2, 2);
                float got = half_to_float(h);
                float diff = std::fabs(got - expected);
                if (diff > maxDiff) maxDiff = diff;
                if (diff > 0.01f) ++badPixels;
            }
        }
        fx.base->UnmapBuffer(readbackBuf);
        std::cout << "[TestVulkanTAA] Frame 1 reproject blend: maxDiff=" << maxDiff
                  << " badPixels=" << badPixels << std::endl;
        TEST_ASSERT(badPixels == 0,
                    "Frame 1 out = 0.9*g(y+1) + 0.1*g(y) (history reproject blend)");
    }

    fx.base->DestroyBuffer(readbackBuf);
    fx.base->DestroyTexture(velTex);
    fx.base->DestroyTexture(inputTex);
    return TestResult::Passed;
}

void RegisterVulkanTAATests() {
    auto suite = std::make_shared<TestSuite>("VulkanTAATests");
    suite->AddTestCase(TestCase("TwoFrameAccumulation", TestTAA_TwoFrameAccumulation));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanTAATests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanTAA] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
