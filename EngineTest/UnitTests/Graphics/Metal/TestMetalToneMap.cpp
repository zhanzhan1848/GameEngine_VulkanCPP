/**
 * @file TestMetalToneMap.cpp
 * @brief Phase 4b Tier 3.4 — Metal reference generator for ToneMap parity.
 * @details Mirrors TestVulkanToneMap.cpp on the Metal backend. Renders a 64×64
 *          HDR gradient (R,G ∈ [0, 4], B=0) through a Metal-embedded MSL shader
 *          that reproduces the WGSL ToneMapping shader's logic exactly:
 *            - Full-screen triangle vertex shader (no vertex buffer)
 *            - ACES tonemap + gamma 1/2.2
 *            - 6-binding argument table: sceneTex, bloomTex, sampler, aoTex,
 *              ssgiTex, velTex (with bloom/ssgi/velocity stubbed to disable
 *              their contributions; AO = white to leave color unmodified).
 *
 *          Saves the result to:
 *            Assets/ReferenceImages/P4b-T3/tonemap_gradient_metal.png
 *
 *          This is the parity reference for TestVulkanToneMap's SSIM check.
 *
 * Convention notes:
 *   - Metal framebuffer is Y-up by default (NDC -1 at bottom, +1 at top).
 *   - Metal texture sampling is Y-down by default (UV (0,0) returns first
 *     pixel of data = top-left in image coords).
 *   - The Metal-rendered PNG is saved WITHOUT Y-flip per the ShadowPass
 *     precedent (TestMetalShadowPass also saves directly).
 *   - The Vulkan test Y-flips its readback before saving, which converts
 *     Vulkan's Y-down framebuffer output to match Metal's Y-up PNG.
 *   - Both should produce visually identical saved PNGs.
 */

#ifdef __APPLE__

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <filesystem>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

namespace {

// ============================= Embedded MSL shader source =============================
// Mirrors ToneMapping.wgsl logic exactly:
//   - Full-screen triangle (no vertex buffer; uses vertex_id)
//   - positions: (-1,-1), (3,-1), (-1,3) — covers the full screen
//   - UVs:       (0,1), (2,1), (0,-1) — stretches beyond [0,1] for the
//     oversized triangle (extra UV region returns OOB clamp = edge)
//   - ACES tonemap + gamma 1/2.2
//   - SSGI_INTENSITY = 0.4 (matches WGSL const)
static const char* kVertexShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOutput vertexMain(uint vertexID [[vertex_id]]) {
    float4 positions[3];
    positions[0] = float4(-1.0, -1.0, 0.0, 1.0);
    positions[1] = float4( 3.0, -1.0, 0.0, 1.0);
    positions[2] = float4(-1.0,  3.0, 0.0, 1.0);
    float2 uvs[3];
    uvs[0] = float2(0.0, 1.0);
    uvs[1] = float2(2.0, 1.0);
    uvs[2] = float2(0.0, -1.0);
    VertexOutput out;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}
)MSL";

static const char* kFragmentShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float SSGI_INTENSITY = 0.4;

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e),
                 float3(0.0), float3(1.0));
}

fragment float4 fragmentMain(
    VertexOutput in [[stage_in]],
    texture2d<float> sceneTexture [[texture(0)]],
    texture2d<float> bloomTexture [[texture(1)]],
    sampler       texSampler      [[sampler(2)]],
    texture2d<float> aoTexture    [[texture(3)]],
    texture2d<float> ssgiTexture  [[texture(4)]],
    texture2d<float> velocityTexture [[texture(5)]]
) {
    float3 color = sceneTexture.sample(texSampler, in.uv).rgb;

    // NaN/Inf guard
    if (any(color != color) || any(abs(color) > float3(3.4e38))) {
        color = float3(0.0);
    }

    // SSAO: darken occluded areas
    float ao = aoTexture.sample(texSampler, in.uv).r;
    color *= ao;

    // SSGI: additive indirect lighting
    float3 ssgi = ssgiTexture.sample(texSampler, in.uv).rgb;
    color += ssgi * SSGI_INTENSITY;

    // Bloom: additive
    float3 bloom = bloomTexture.sample(texSampler, in.uv).rgb;
    float3 result = color + bloom;

    // Tone map + gamma
    result = ACESFilm(result);
    result = pow(result, float3(1.0 / 2.2));

    return float4(result, 1.0);
}
)MSL";

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

constexpr u32 kW = 64;
constexpr u32 kH = 64;

// ============================= HDR gradient (matches Vulkan test) =============================
void generate_hdr_gradient(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);
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

} // anonymous namespace

// ============================= Main test =============================
TestResult TestToneMap_RenderGradient_Metal() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        std::cerr << "[TestMetalToneMap] MetalDevice::Initialize failed" << std::endl;
        return TestResult::Failed;
    }

    // ----- 1. Shaders (embedded MSL) -----
    ShaderHandle vs = device.CreateShader(
        kVertexShaderSource, std::strlen(kVertexShaderSource),
        ShaderStage::Vertex, "vertexMain");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
    ShaderHandle fs = device.CreateShader(
        kFragmentShaderSource, std::strlen(kFragmentShaderSource),
        ShaderStage::Pixel, "fragmentMain");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

    // ----- 2. Scene HDR gradient + 4 stub textures -----
    std::vector<u8> scenePixels;
    generate_hdr_gradient(scenePixels);

    BufferDesc sceneStagingDesc{};
    sceneStagingDesc.size = scenePixels.size();
    sceneStagingDesc.type = BufferType::Raw;
    sceneStagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    sceneStagingDesc.name = "SceneStaging";
    ResourceHandle sceneStaging = device.CreateBuffer(sceneStagingDesc);
    TEST_ASSERT(sceneStaging != handles::INVALID_RESOURCE, "CreateBuffer sceneStaging");
    TEST_ASSERT(device.UpdateBufferData(sceneStaging, scenePixels.data(), scenePixels.size(), 0),
                "UpdateBufferData sceneStaging");

    TextureDesc sceneDesc{};
    sceneDesc.size = { kW, kH, 1 };
    sceneDesc.mipLevels = 1;
    sceneDesc.arraySize = 1;
    sceneDesc.format = DataFormat::RGBA16_Float;
    sceneDesc.type = TextureType::Texture2D;
    sceneDesc.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
    sceneDesc.memoryUsage = GPUMemoryUsage::Static;
    sceneDesc.name = "ToneMap_Scene";
    ResourceHandle sceneTex = device.CreateTexture(sceneDesc);
    TEST_ASSERT(sceneTex != handles::INVALID_RESOURCE, "CreateTexture sceneTex");

    {
        CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
        TEST_ASSERT(mcmd->Reset(), "Reset sceneStaging");
        TEST_ASSERT(mcmd->Begin(), "Begin sceneStaging");
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = { kW, kH, 1 };
        mcmd->CopyBufferToTexture(sceneStaging, sceneTex, &region, 1);
        ResourceBarrier b{};
        b.resource = sceneTex;
        b.beforeState = ResourceState::CopyDest;
        b.afterState = ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        mcmd->InsertBarrier(&b, 1);
        TEST_ASSERT(mcmd->End(), "End sceneStaging");
        TEST_ASSERT(mcmd->Submit(0), "Submit sceneStaging");
        TEST_ASSERT(mcmd->WaitForCompletion(), "WaitForCompletion sceneStaging");
        device.DestroyCommandBuffer(cmd);
    }

    // Stub textures (1×1)
    auto create_stub = [&](const std::vector<u8>& px, DataFormat fmt, const char* name) -> ResourceHandle {
        BufferDesc sb{};
        sb.size = px.size();
        sb.type = BufferType::Raw;
        sb.memoryUsage = GPUMemoryUsage::Dynamic;
        sb.name = "StubStaging";
        ResourceHandle staging = device.CreateBuffer(sb);
        if (staging == handles::INVALID_RESOURCE) return staging;
        device.UpdateBufferData(staging, px.data(), px.size(), 0);

        TextureDesc td{};
        td.size = { 1, 1, 1 };
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = fmt;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = name;
        ResourceHandle tex = device.CreateTexture(td);

        CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
        mcmd->Reset(); mcmd->Begin();
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = { 1, 1, 1 };
        mcmd->CopyBufferToTexture(staging, tex, &region, 1);
        ResourceBarrier b{};
        b.resource = tex;
        b.beforeState = ResourceState::CopyDest;
        b.afterState = ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        mcmd->InsertBarrier(&b, 1);
        mcmd->End(); mcmd->Submit(0); mcmd->WaitForCompletion();
        device.DestroyCommandBuffer(cmd);
        device.DestroyBuffer(staging);
        return tex;
    };

    std::vector<u8> black16, white8, black16_2, black16_3;
    make_rgba16f_pixel(black16,   0.0f, 0.0f, 0.0f, 1.0f);  // bloom = 0
    make_rgba8_pixel   (white8,   255,  255,  255,  255);    // AO = 1
    make_rgba16f_pixel(black16_2, 0.0f, 0.0f, 0.0f, 1.0f);  // SSGI = 0
    make_rgba16f_pixel(black16_3, 0.0f, 0.0f, 0.0f, 1.0f);  // velocity = 0

    ResourceHandle bloomTex = create_stub(black16,   DataFormat::RGBA16_Float, "ToneMap_Bloom");
    ResourceHandle aoTex    = create_stub(white8,    DataFormat::RGBA8_UNorm,  "ToneMap_AO");
    ResourceHandle ssgiTex  = create_stub(black16_2, DataFormat::RGBA16_Float, "ToneMap_SSGI");
    ResourceHandle velTex   = create_stub(black16_3, DataFormat::RGBA16_Float, "ToneMap_Vel");
    TEST_ASSERT(bloomTex != handles::INVALID_RESOURCE, "CreateTexture bloomTex");
    TEST_ASSERT(aoTex    != handles::INVALID_RESOURCE, "CreateTexture aoTex");
    TEST_ASSERT(ssgiTex  != handles::INVALID_RESOURCE, "CreateTexture ssgiTex");
    TEST_ASSERT(velTex   != handles::INVALID_RESOURCE, "CreateTexture velTex");

    // ----- 3. Sampler (linear + clamp — matches Vulkan test) -----
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.maxAnisotropy = 1;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = device.CreateSampler(samplerDesc);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler");

    // ----- 4. Descriptor set: 6 bindings -----
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
    DescriptorSetLayoutHandle layout = device.CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = device.CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = device.CreateDescriptorSet(dsDesc);
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
    device.UpdateDescriptorSets(6, writes);

    // ----- 5. Graphics pipeline -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = device.CreateGraphicsPipeline(gpd);
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
    ResourceHandle rt = device.CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RGBA8 RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 4;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "ToneMap_Readback";
    ResourceHandle readback = device.CreateBuffer(readbackDesc);
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

    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd->Reset(), "Reset");
    TEST_ASSERT(mcmd->Begin(), "Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    mcmd->Draw(3, 0, 1, 0);
    mcmd->EndRenderPass();

    ResourceBarrier rtBarrier{};
    rtBarrier.resource = rt;
    rtBarrier.beforeState = ResourceState::RenderTarget;
    rtBarrier.afterState = ResourceState::CopySource;
    rtBarrier.subresource = 0xFFFFFFFF;
    rtBarrier.queueFamily = 0xFFFFFFFF;
    mcmd->InsertBarrier(&rtBarrier, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    mcmd->CopyTextureToBuffer(rt, readback, &region, 1);

    TEST_ASSERT(mcmd->End(), "End");
    TEST_ASSERT(mcmd->Submit(0), "Submit");
    TEST_ASSERT(mcmd->WaitForCompletion(), "WaitForCompletion");
    device.DestroyCommandBuffer(cmd);

    // ----- 8. Readback + save PNG (no Y-flip — Metal convention) -----
    void* mapped = device.MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
    std::vector<u8> rgba8(static_cast<const u8*>(mapped),
                          static_cast<const u8*>(mapped) + readbackDesc.size);
    device.UnmapBuffer(readback);

    u32 brightPx = 0;
    for (u32 i = 0; i < kW * kH; ++i) {
        u8 r = rgba8[i * 4];
        if (r > 200) ++brightPx;
    }
    std::cout << "[TestMetalToneMap] bright (R>200) pixels = " << brightPx
              << " / " << (kW * kH) << std::endl;

    std::filesystem::create_directories("Assets/ReferenceImages/P4b-T3");
    const char* refPath = "Assets/ReferenceImages/P4b-T3/tonemap_gradient_metal.png";
    if (!et::SavePNG(refPath, rgba8.data(), kW, kH)) {
        std::cerr << "[TestMetalToneMap] SavePNG failed for " << refPath << std::endl;
        return TestResult::Failed;
    }
    std::cout << "[TestMetalToneMap] saved " << refPath << std::endl;

    // Cleanup
    device.DestroyBuffer(readback);
    device.DestroyTexture(rt);
    device.DestroyPipeline(pipe);
    device.DestroyPipelineLayout(pl);
    device.DestroyDescriptorSet(ds);
    device.DestroyDescriptorSetLayout(layout);
    device.DestroySampler(sampler);
    device.DestroyTexture(velTex);
    device.DestroyTexture(ssgiTex);
    device.DestroyTexture(aoTex);
    device.DestroyTexture(bloomTex);
    device.DestroyTexture(sceneTex);
    device.DestroyBuffer(sceneStaging);
    device.DestroyShader(fs);
    device.DestroyShader(vs);
    device.Shutdown();
    return TestResult::Passed;
}

void RegisterMetalToneMapTests() {
    auto suite = std::make_shared<TestSuite>("MetalToneMapTests");
    suite->AddTestCase(TestCase("RenderGradient_Metal", TestToneMap_RenderGradient_Metal));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterMetalToneMapTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

int main() {
    std::cout << "[TestMetalToneMap] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif // __APPLE__
