/**
 * @file TestVulkanToneMap.cpp
 * @brief Phase 4b Tier 3.4 — ToneMap port (ACES + gamma, full-screen triangle).
 * @details Renders a 64×64 HDR gradient (R,G ∈ [0, 4]) through ToneMapping.spv
 *          (WGSL-compiled ACES tonemap shader) into an RGBA8 LDR target. Reads
 *          back, flips Y (Vulkan framebuffer is Y-down; Metal is Y-up), saves
 *          PNG, and SSIM-compares against the Metal reference.
 *
 *          All "auxiliary" inputs (bloom/ao/ssgi/velocity) are 1×1 stubs that
 *          disable their respective effects — we only test the ACES curve path.
 *
 * Validates:
 *   - Multi-entry-point SPIR-V (tonemap_vs + tonemap_fs in same module, loaded
 *     twice with different entry-point names).
 *   - 6-binding descriptor set (3 textures + 1 sampler + 2 more textures).
 *   - Full-screen triangle vertex shader (no vertex buffer; uses vertex_index).
 *   - Texture sampling with linear filter (Vulkan vs Metal sampler parity).
 *   - ACES tone map + gamma curve numerical correctness.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
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

constexpr u32 kW = 64;
constexpr u32 kH = 64;

// ============================= Half-float helpers (for HDR scene tex) =============================
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

// ============================= HDR input gradient (R,G ∈ [0, 4], B=0) =============================
// Each pixel's RGB encodes an HDR value that exercises the ACES curve:
//   R = x/W * 4.0  (0 at left edge → 4 at right edge)
//   G = y/H * 4.0  (0 at top row → 4 at bottom row in texture-space)
//   B = 0
//   A = 1
// After ACES: highlights compress; gamma 1/2.2 brings linear values to sRGB.
void generate_hdr_gradient(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);  // RGBA16F = 8 bytes/pixel
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float r = float(x) / float(kW - 1) * 4.0f;
            float g = float(y) / float(kH - 1) * 4.0f;
            float b = 0.0f;
            float a = 1.0f;
            u16 hr = float_to_half(r), hg = float_to_half(g),
                hb = float_to_half(b), ha = float_to_half(a);
            u8* p = out.data() + (size_t(y) * kW + x) * 8;
            std::memcpy(p + 0, &hr, 2);
            std::memcpy(p + 2, &hg, 2);
            std::memcpy(p + 4, &hb, 2);
            std::memcpy(p + 6, &ha, 2);
        }
    }
}

// ============================= 1×1 stub generators =============================
void make_rgba16f_pixel(std::vector<u8>& out, float r, float g, float b, float a) {
    out.resize(8);
    u16 hr = float_to_half(r), hg = float_to_half(g),
        hb = float_to_half(b), ha = float_to_half(a);
    std::memcpy(out.data() + 0, &hr, 2);
    std::memcpy(out.data() + 2, &hg, 2);
    std::memcpy(out.data() + 4, &hb, 2);
    std::memcpy(out.data() + 6, &ha, 2);
}

void make_rgba8_pixel(std::vector<u8>& out, u8 r, u8 g, u8 b, u8 a) {
    out = { r, g, b, a };
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

// Helper: create a 2D texture, upload pixel data via staging, transition to ShaderResource.
// Returns INVALID_RESOURCE on failure.
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
    if (staging == handles::INVALID_RESOURCE || !fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0)) {
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

TestResult TestToneMap_RenderGradient() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ----- 1. Shader (single SPIR-V with both entry points) -----
    auto spv = ReadSPV("Assets/Shaders/ToneMapping.spv");
    TEST_ASSERT(!spv.empty(), "Read ToneMapping SPIR-V (run build_spv.sh if missing)");
    ShaderHandle vs = fx.base->CreateShader(spv.data(), spv.size(),
                                            ShaderStage::Vertex, "tonemap_vs");
    ShaderHandle fs = fx.base->CreateShader(spv.data(), spv.size(),
                                            ShaderStage::Pixel, "tonemap_fs");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex (tonemap_vs)");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment (tonemap_fs)");

    // ----- 2. Scene HDR gradient + 4 stub textures -----
    std::vector<u8> scenePixels;
    generate_hdr_gradient(scenePixels);

    TextureDesc sceneDesc{};
    sceneDesc.size = { kW, kH, 1 };
    sceneDesc.mipLevels = 1;
    sceneDesc.arraySize = 1;
    sceneDesc.format = DataFormat::RGBA16_Float;
    sceneDesc.type = TextureType::Texture2D;
    ResourceHandle sceneTex = CreateAndUploadTexture(fx, sceneDesc, scenePixels, "ToneMap_Scene");
    TEST_ASSERT(sceneTex != handles::INVALID_RESOURCE, "CreateTexture sceneTex");

    // Stubs: 1×1 textures to disable bloom/ao/ssgi/velocity contributions.
    TextureDesc stub16Desc{};
    stub16Desc.size = { 1, 1, 1 };
    stub16Desc.mipLevels = 1;
    stub16Desc.arraySize = 1;
    stub16Desc.format = DataFormat::RGBA16_Float;
    stub16Desc.type = TextureType::Texture2D;
    TextureDesc stub8Desc = stub16Desc;
    stub8Desc.format = DataFormat::RGBA8_UNorm;

    std::vector<u8> black16, black16_2, white8, black16_3;
    make_rgba16f_pixel(black16,   0.0f, 0.0f, 0.0f, 1.0f);  // bloom = 0
    make_rgba8_pixel   (white8,   255,  255,  255,  255);    // AO = 1
    make_rgba16f_pixel(black16_2, 0.0f, 0.0f, 0.0f, 1.0f);  // SSGI = 0
    make_rgba16f_pixel(black16_3, 0.0f, 0.0f, 0.0f, 1.0f);  // velocity = 0

    ResourceHandle bloomTex = CreateAndUploadTexture(fx, stub16Desc, black16,   "ToneMap_Bloom");
    ResourceHandle aoTex    = CreateAndUploadTexture(fx, stub8Desc,  white8,    "ToneMap_AO");
    ResourceHandle ssgiTex  = CreateAndUploadTexture(fx, stub16Desc, black16_2, "ToneMap_SSGI");
    ResourceHandle velTex   = CreateAndUploadTexture(fx, stub16Desc, black16_3, "ToneMap_Vel");
    TEST_ASSERT(bloomTex != handles::INVALID_RESOURCE, "CreateTexture bloomTex");
    TEST_ASSERT(aoTex    != handles::INVALID_RESOURCE, "CreateTexture aoTex");
    TEST_ASSERT(ssgiTex  != handles::INVALID_RESOURCE, "CreateTexture ssgiTex");
    TEST_ASSERT(velTex   != handles::INVALID_RESOURCE, "CreateTexture velTex");

    // ----- 3. Sampler (linear + clamp — same as Metal reference) -----
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.maxAnisotropy = 1;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(samplerDesc);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler");

    // ----- 4. Descriptor set: 6 bindings -----
    //   0: sceneTex (SampledImage, RGBA16F)
    //   1: bloomTex (SampledImage, RGBA16F)
    //   2: sampler  (Sampler)
    //   3: aoTex    (SampledImage, RGBA8)
    //   4: ssgiTex  (SampledImage, RGBA16F)
    //   5: velTex   (SampledImage, RGBA16F)
    DescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr };
    bindings[1] = { 1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr };
    bindings[2] = { 2, DescriptorType::Sampler,     1, ShaderStage::Pixel, nullptr };
    bindings[3] = { 3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr };
    bindings[4] = { 4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr };
    bindings[5] = { 5, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 6;
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

    DescriptorImageInfo sceneInfo{ handles::INVALID_SAMPLER, sceneTex, ResourceState::ShaderResource };
    DescriptorImageInfo bloomInfo{ handles::INVALID_SAMPLER, bloomTex, ResourceState::ShaderResource };
    DescriptorImageInfo sampInfo{ sampler, handles::INVALID_RESOURCE, ResourceState::Unknown };
    DescriptorImageInfo aoInfo{   handles::INVALID_SAMPLER, aoTex,    ResourceState::ShaderResource };
    DescriptorImageInfo ssgiInfo{ handles::INVALID_SAMPLER, ssgiTex,  ResourceState::ShaderResource };
    DescriptorImageInfo velInfo{  handles::INVALID_SAMPLER, velTex,   ResourceState::ShaderResource };

    WriteDescriptorSet writes[6]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage, &sceneInfo, nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::SampledImage, &bloomInfo, nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::Sampler,      &sampInfo,  nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::SampledImage, &aoInfo,    nullptr };
    writes[4] = { ds, 4, 0, 1, DescriptorType::SampledImage, &ssgiInfo,  nullptr };
    writes[5] = { ds, 5, 0, 1, DescriptorType::SampledImage, &velInfo,   nullptr };
    fx.base->UpdateDescriptorSets(6, writes);

    // ----- 5. Graphics pipeline -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;  // full-screen triangle, no cull
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline (tonemap)");

    // ----- 6. RGBA8 output RT + readback buffer -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA8_UNorm;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "ToneMap_RT";
    ResourceHandle rt = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RGBA8 RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 4;  // RGBA8 = 4 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "ToneMap_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // ----- 7. Render -----
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = { 0.0f, 0.0f, 0.0f, 1.0f };
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Draw(3, 0, 1, 0);  // 3 vertices, startVertex=0, 1 instance, startInstance=0
    vcmd->EndRenderPass();

    // RT: RenderTarget → CopySource
    ResourceBarrier rtBarrier{};
    rtBarrier.resource = rt;
    rtBarrier.beforeState = ResourceState::RenderTarget;
    rtBarrier.afterState = ResourceState::CopySource;
    rtBarrier.subresource = 0xFFFFFFFF;
    rtBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&rtBarrier, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(rt, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // ----- 8. Readback (NO Y-flip — naga inserts Y-negate on gl_Position.y
    // for WGSL→SPIR-V, which already orients the framebuffer to match Metal's
    // Y-up convention. Adding FlipYInPlace here would actually flip it wrong.) -----
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
    std::vector<u8> rgba8(static_cast<const u8*>(mapped),
                          static_cast<const u8*>(mapped) + readbackDesc.size);
    fx.base->UnmapBuffer(readback);

    // Sanity: stats on output
    u32 brightPx = 0;
    for (u32 i = 0; i < kW * kH; ++i) {
        u8 r = rgba8[i * 4];
        if (r > 200) ++brightPx;
    }
    std::cout << "[TestVulkanToneMap] bright (R>200) pixels = " << brightPx
              << " / " << (kW * kH) << std::endl;
    // The rightmost column has R_input = 4.0 (HDR max). After ACES + gamma,
    // R should approach ~248. At least the rightmost column should be bright.
    TEST_ASSERT(brightPx > kH, "Bright column visible (ACES highlights compressed)");

    // Save layer 0 for inspection
    const char* outPath = "tonemap_gradient_vulkan.png";
    if (et::SavePNG(outPath, rgba8.data(), kW, kH)) {
        std::cout << "[TestVulkanToneMap] saved " << outPath << std::endl;
    }

    // ----- 9. SSIM vs Metal reference -----
    const char* refPath = "Assets/ReferenceImages/P4b-T3/tonemap_gradient_metal.png";
    std::vector<u8> refRgba;
    u32 refW = 0, refH = 0;
    if (et::LoadPNG(refPath, refRgba, refW, refH)) {
        TEST_ASSERT(refW == kW && refH == kH, "Metal reference dimensions match");
        float ssim = et::ComputeSSIM(rgba8.data(), refRgba.data(), kW, kH);
        std::cout << "[TestVulkanToneMap] SSIM vs Metal: " << ssim << std::endl;
        TEST_ASSERT(ssim >= 0.95f, "SSIM >= 0.95 vs Metal tonemap reference");
    } else {
        std::cerr << "[TestVulkanToneMap] Metal reference missing at " << refPath
                  << " — skipping SSIM" << std::endl;
    }

    // Cleanup
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyTexture(velTex);
    fx.base->DestroyTexture(ssgiTex);
    fx.base->DestroyTexture(aoTex);
    fx.base->DestroyTexture(bloomTex);
    fx.base->DestroyTexture(sceneTex);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanToneMapTests() {
    auto suite = std::make_shared<TestSuite>("VulkanToneMapTests");
    suite->AddTestCase(TestCase("RenderGradient", TestToneMap_RenderGradient));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanToneMapTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanToneMap] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
