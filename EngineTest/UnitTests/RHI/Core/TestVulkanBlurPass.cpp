/**
 * @file TestVulkanBlurPass.cpp
 * @brief Phase 4b Tier 3.3 — BlurPass compute port.
 * @details Dispatches BlurPass.spv (WGSL-compiled 5-tap separable Gaussian)
 *          on a 32×32 RGBA16F texture with a known bright-cross pattern,
 *          reads back the result, and compares against a CPU-computed
 *          reference that mirrors the WGSL shader's exact semantics
 *          (including unsigned-wrap on negative texture coords → vec4(0)).
 *
 * Validates:
 *   - Vulkan compute pipeline + descriptor set with mixed bindings:
 *       binding 0 = SampledImage (input tex, RGBA16F, ShaderResource)
 *       binding 1 = StorageImage (output tex, RGBA16F, UnorderedAccess)
 *       binding 2 = UniformBuffer (BlurParams, 16 B)
 *   - textureLoad OOB returns vec4(0) (WGSL semantics) — CPU reference matches.
 *   - Half-float (RGBA16F) round-trip precision in compute output.
 *
 * Tolerance: per-pixel max abs diff < 0.05 across all 4 RGBA channels.
 *   Half-float has ~10-bit mantissa; for HDR values up to ~5.0, precision
 *   is ~0.005, so 0.05 gives a 10× safety margin for accumulated math error.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 32;
constexpr u32 kH = 32;
constexpr u32 kWorkgroupSize = 8;

// ============================= Half-float helpers =============================
// IEEE 754 half-float (binary16) ↔ single-precision (binary32).
// Matches what GPU produces for RGBA16F storage.
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
        u16 out = static_cast<u16>(sign | (mant >> shift));
        return out;
    } else if (exp == 0xFF - (127 - 15)) {
        return static_cast<u16>(sign | 0x7C00 | (mant >> 13));
    } else if (exp > 31) {
        return static_cast<u16>(sign | 0x7C00);  // Inf
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

float half_to_float(u16 h) {
    u32 sign = (h & 0x8000) << 16;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {
            exp = 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FF;
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Pack/unpack 4-component RGBA half-float pixels.
void pack_rgba16f(u8* dst, const float rgba[4]) {
    for (int c = 0; c < 4; ++c) {
        u16 h = float_to_half(rgba[c]);
        std::memcpy(dst + c * 2, &h, 2);
    }
}

void unpack_rgba16f(const u8* src, float rgba[4]) {
    for (int c = 0; c < 4; ++c) {
        u16 h;
        std::memcpy(&h, src + c * 2, 2);
        rgba[c] = half_to_float(h);
    }
}

// ============================= Input pattern =============================
// Bright cross centered in the texture, surrounded by dark.
// Center pixel (16,16) is HDR (5.0, 3.0, 1.0, 1.0) — exercises half-float range.
// Cross arms (radius 4 from center along axes): medium brightness (2.0, 1.0, 0.5, 1.0)
// All other pixels: dark (0.1, 0.1, 0.2, 1.0)
void generate_input_pattern(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float rgba[4];
            int32_t dx = static_cast<int32_t>(x) - 16;
            int32_t dy = static_cast<int32_t>(y) - 16;
            bool onCross =
                (std::abs(dx) <= 4 && dy == 0) ||
                (std::abs(dy) <= 4 && dx == 0);
            if (dx == 0 && dy == 0) {
                rgba[0] = 5.0f; rgba[1] = 3.0f; rgba[2] = 1.0f; rgba[3] = 1.0f;
            } else if (onCross) {
                rgba[0] = 2.0f; rgba[1] = 1.0f; rgba[2] = 0.5f; rgba[3] = 1.0f;
            } else {
                rgba[0] = 0.1f; rgba[1] = 0.1f; rgba[2] = 0.2f; rgba[3] = 1.0f;
            }
            pack_rgba16f(out.data() + (size_t(y) * kW + x) * 8, rgba);
        }
    }
}

// ============================= CPU reference (matches WGSL shader) =============================
// Weights from BlurPass.wgsl (5-tap separable Gaussian):
//   center: 0.2270270270
//   ±1    : 0.3162162162 each
//   ±2    : 0.070270270  each
// Sum = 0.999999... ≈ 1.0 (normalization).
//
// IMPORTANT: WGSL uses u32 subtraction for `texCoord - offset`, which wraps
// to a large u32 on underflow. textureLoad with OOB coord returns vec4(0).
// The CPU reference must mirror this — unsigned-wrap then clamp to "no sample".
struct BlurParams { u32 direction; u32 mipLevel; u32 pad0; u32 pad1; };

void cpu_blur(const std::vector<u8>& input, std::vector<u8>& output, const BlurParams& params) {
    output.resize(size_t(kW) * kH * 8);
    // Weights — keep full float precision (don't quantize to half yet).
    constexpr float wCenter = 0.2270270270f;
    constexpr float wNear   = 0.3162162162f;
    constexpr float wFar    = 0.070270270f;

    int32_t offsetX = (params.direction == 1u) ? 0 : 1;
    int32_t offsetY = (params.direction == 1u) ? 1 : 0;

    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            // Mirror the WGSL unsigned-wrap lookup: u32 wrap, then OOB → vec4(0).
            auto sampleAt = [&](u32 sx, u32 sy) -> std::array<float, 4> {
                if (sx >= kW || sy >= kH) return {0.0f, 0.0f, 0.0f, 0.0f};
                float rgba[4];
                unpack_rgba16f(input.data() + (size_t(sy) * kW + sx) * 8, rgba);
                return {rgba[0], rgba[1], rgba[2], rgba[3]};
            };

            // unsigned subtraction: x - 1u wraps to 0xFFFFFFFF when x == 0.
            u32 px1 = x + static_cast<u32>(offsetX);
            u32 mx1 = x - static_cast<u32>(offsetX);
            u32 py1 = y + static_cast<u32>(offsetY);
            u32 my1 = y - static_cast<u32>(offsetY);
            u32 px2 = x + 2u * static_cast<u32>(offsetX);
            u32 mx2 = x - 2u * static_cast<u32>(offsetX);
            u32 py2 = y + 2u * static_cast<u32>(offsetY);
            u32 my2 = y - 2u * static_cast<u32>(offsetY);

            auto center = sampleAt(x, y);
            auto right1 = sampleAt(px1, py1);
            auto left1  = sampleAt(mx1, my1);
            auto right2 = sampleAt(px2, py2);
            auto left2  = sampleAt(mx2, my2);

            std::array<float, 4> sum{};
            for (int c = 0; c < 4; ++c) {
                sum[c] = center[c] * wCenter
                       + right1[c] * wNear
                       + left1[c]  * wNear
                       + right2[c] * wFar
                       + left2[c]  * wFar;
            }
            float rgbaArr[4] = {sum[0], sum[1], sum[2], sum[3]};
            pack_rgba16f(output.data() + (size_t(y) * kW + x) * 8, rgbaArr);
        }
    }
}

// ============================= SPIR-V loader =============================
std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "ReadSPV: cannot open " << path << std::endl;
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
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

} // anonymous namespace

TestResult TestBlurPass_DispatchAndCompare() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ----- 1. Shader -----
    auto cs = ReadSPV("Assets/Shaders/BlurPass.spv");
    TEST_ASSERT(!cs.empty(), "Read BlurPass SPIR-V (run build_spv.sh if missing)");
    ShaderHandle computeShader = fx.base->CreateShader(
        cs.data(), cs.size(), ShaderStage::Compute, "blurCS");
    TEST_ASSERT(computeShader != handles::INVALID_SHADER, "CreateShader compute (BlurPass)");

    // ----- 2. Input pattern (32×32 RGBA16F) -----
    std::vector<u8> inputBytes;
    generate_input_pattern(inputBytes);

    TextureDesc inDesc{};
    inDesc.size = { kW, kH, 1 };
    inDesc.mipLevels = 1;
    inDesc.arraySize = 1;
    inDesc.format = DataFormat::RGBA16_Float;
    inDesc.type = TextureType::Texture2D;
    inDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    inDesc.memoryUsage = GPUMemoryUsage::Static;
    inDesc.name = "BlurPass_Input";
    ResourceHandle inTex = fx.base->CreateTexture(inDesc);
    TEST_ASSERT(inTex != handles::INVALID_RESOURCE, "CreateTexture input");

    // Upload via staging
    {
        BufferDesc stagingDesc{};
        stagingDesc.size = inputBytes.size();
        stagingDesc.type = BufferType::Raw;
        stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        stagingDesc.name = "BlurPass_Staging";
        ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
        TEST_ASSERT(staging != handles::INVALID_RESOURCE, "CreateBuffer staging");
        TEST_ASSERT(fx.base->UpdateBufferData(staging, inputBytes.data(), inputBytes.size(), 0),
                    "UpdateBufferData staging");

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin staging cmd");

        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kW, kH, 1};
        vcmd->CopyBufferToTexture(staging, inTex, &region, 1);

        ResourceBarrier b{};
        b.resource = inTex;
        b.beforeState = ResourceState::CopyDest;
        b.afterState = ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&b, 1);

        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit staging");
        fx.base->DestroyCommandBuffer(cmd);
        fx.base->DestroyBuffer(staging);
    }

    // ----- 3. Output texture (32×32 RGBA16F, UnorderedAccess) -----
    TextureDesc outDesc = inDesc;
    outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    outDesc.name = "BlurPass_Output";
    ResourceHandle outTex = fx.base->CreateTexture(outDesc);
    TEST_ASSERT(outTex != handles::INVALID_RESOURCE, "CreateTexture output");

    // ----- 4. BlurParams UBO (direction=0 horizontal) -----
    BlurParams params{ /*direction*/ 0u, /*mipLevel*/ 0u, 0u, 0u };
    BufferDesc ubufDesc{};
    ubufDesc.size = sizeof(BlurParams);  // 16
    ubufDesc.type = BufferType::Constant;
    ubufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubufDesc.name = "BlurPass_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(ubufDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer BlurParams");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &params, sizeof(params), 0),
                "UpdateBufferData BlurParams");

    // ----- 5. Descriptor set -----
    DescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = DescriptorType::SampledImage;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Compute;
    bindings[1].binding = 1;
    bindings[1].descriptorType = DescriptorType::StorageImage;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Compute;
    bindings[2].binding = 2;
    bindings[2].descriptorType = DescriptorType::UniformBuffer;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 3;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_RESOURCE, "CreateDescriptorSet");

    DescriptorImageInfo inInfo{};
    inInfo.imageView = inTex;
    inInfo.imageLayout = ResourceState::ShaderResource;
    DescriptorImageInfo outInfo{};
    outInfo.imageView = outTex;
    outInfo.imageLayout = ResourceState::UnorderedAccess;
    DescriptorBufferInfo uboInfo{};
    uboInfo.buffer = ubo;
    uboInfo.offset = 0;
    uboInfo.range = sizeof(BlurParams);
    WriteDescriptorSet writes[3]{};
    writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
    writes[0].imageInfo = &inInfo;
    writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::StorageImage;
    writes[1].imageInfo = &outInfo;
    writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].dstArrayElement = 0;
    writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::UniformBuffer;
    writes[2].bufferInfo = &uboInfo;
    fx.base->UpdateDescriptorSets(3, writes);

    // ----- 6. Compute pipeline -----
    ComputePipelineDesc cpd{};
    cpd.computeShader = computeShader;
    cpd.layout = pl;
    cpd.threadGroupSize = { kWorkgroupSize, kWorkgroupSize, 1 };
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline (BlurPass)");

    // ----- 7. Dispatch -----
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin dispatch cmd");

    // Output starts UnorderedAccess (general layout for storage write).
    // Input is ShaderResource (already transitioned during staging upload).
    // Output texture is fresh (UNDEFINED layout) — must transition to GENERAL
    // before the descriptor is accessed by the compute shader.
    ResourceBarrier initOutBarrier{};
    initOutBarrier.resource = outTex;
    initOutBarrier.beforeState = ResourceState::Unknown;  // == UNDEFINED
    initOutBarrier.afterState = ResourceState::UnorderedAccess;
    initOutBarrier.subresource = 0xFFFFFFFF;
    initOutBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initOutBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    u32 gx = (kW + kWorkgroupSize - 1) / kWorkgroupSize;
    u32 gy = (kH + kWorkgroupSize - 1) / kWorkgroupSize;
    vcmd->Dispatch(gx, gy, 1);

    // Compute write → transfer read sync.
    vcmd->MemoryBarrier(
        PipelineStage::ComputeShader,
        PipelineStage::Transfer,
        AccessFlag::ShaderWrite,
        AccessFlag::TransferRead);

    // Output: UnorderedAccess → CopySource
    ResourceBarrier outBarrier{};
    outBarrier.resource = outTex;
    outBarrier.beforeState = ResourceState::UnorderedAccess;
    outBarrier.afterState = ResourceState::CopySource;
    outBarrier.subresource = 0xFFFFFFFF;
    outBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&outBarrier, 1);

    // Copy output to readback
    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 8;  // RGBA16F = 8 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "BlurPass_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(outTex, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit dispatch");
    fx.base->DestroyCommandBuffer(cmd);

    // ----- 8. Readback + CPU reference + compare -----
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
    std::vector<u8> gpuOutput(static_cast<const u8*>(mapped),
                              static_cast<const u8*>(mapped) + readbackDesc.size);
    fx.base->UnmapBuffer(readback);

    std::vector<u8> cpuOutput;
    cpu_blur(inputBytes, cpuOutput, params);

    // Per-pixel comparison (RGBA channels, float domain).
    float maxAbsDiff = 0.0f;
    double sumAbsDiff = 0.0;
    u32 worstPixel = 0xFFFFFFFF;
    float worstGpu[4]{}, worstCpu[4]{};
    for (u32 i = 0; i < kW * kH; ++i) {
        float gpu[4], cpu[4];
        unpack_rgba16f(gpuOutput.data() + i * 8, gpu);
        unpack_rgba16f(cpuOutput.data() + i * 8, cpu);
        for (int c = 0; c < 4; ++c) {
            float diff = std::fabs(gpu[c] - cpu[c]);
            if (diff > maxAbsDiff) {
                maxAbsDiff = diff;
                worstPixel = i;
                std::memcpy(worstGpu, gpu, sizeof(worstGpu));
                std::memcpy(worstCpu, cpu, sizeof(worstCpu));
            }
            sumAbsDiff += diff;
        }
    }
    double meanAbsDiff = sumAbsDiff / (double(kW) * kH * 4);

    std::cout << "[TestVulkanBlurPass] max abs diff = " << maxAbsDiff << std::endl;
    std::cout << "[TestVulkanBlurPass] mean abs diff = " << meanAbsDiff << std::endl;
    if (worstPixel != 0xFFFFFFFF) {
        u32 wx = worstPixel % kW, wy = worstPixel / kW;
        std::cout << "[TestVulkanBlurPass] worst pixel (" << wx << "," << wy << "):"
                  << " gpu=(" << worstGpu[0] << "," << worstGpu[1] << ","
                  << worstGpu[2] << "," << worstGpu[3] << ")"
                  << " cpu=(" << worstCpu[0] << "," << worstCpu[1] << ","
                  << worstCpu[2] << "," << worstCpu[3] << ")" << std::endl;
    }

    // Tolerance: half-float precision ~0.005 for values <5.0; 0.05 is 10× margin.
    constexpr float kMaxDiff = 0.05f;
    TEST_ASSERT(maxAbsDiff < kMaxDiff, "Max abs diff < 0.05 (half-float + accumulated math)");

    // Sanity: the blur actually did something. Center pixel brightness should
    // propagate outward — compare to a "no blur" scenario where output==input.
    // Pick a pixel adjacent to the bright center: it should differ noticeably
    // from its input value (which was the dark or cross color).
    {
        u32 px = 18, py = 16;  // 2 px right of center — was on cross (medium)
        float gpuIn[4], gpuOut[4];
        unpack_rgba16f(inputBytes.data() + (py * kW + px) * 8, gpuIn);
        unpack_rgba16f(gpuOutput.data() + (py * kW + px) * 8, gpuOut);
        float inputLum = 0.2126f * gpuIn[0] + 0.7152f * gpuIn[1] + 0.0722f * gpuIn[2];
        float outputLum = 0.2126f * gpuOut[0] + 0.7152f * gpuOut[1] + 0.0722f * gpuOut[2];
        std::cout << "[TestVulkanBlurPass] pixel (" << px << "," << py << "):"
                  << " in_lum=" << inputLum << " out_lum=" << outputLum << std::endl;
        TEST_ASSERT(std::fabs(outputLum - inputLum) > 0.1f,
                    "Blur must produce visible change at neighbor pixel");
    }

    // Cleanup
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(outTex);
    fx.base->DestroyTexture(inTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyShader(computeShader);
    return TestResult::Passed;
}

void RegisterVulkanBlurPassTests() {
    auto suite = std::make_shared<TestSuite>("VulkanBlurPassTests");
    suite->AddTestCase(TestCase("DispatchAndCompare", TestBlurPass_DispatchAndCompare));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanBlurPassTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanBlurPass] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
