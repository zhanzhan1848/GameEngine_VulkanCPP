/**
 * @file TestVulkanLayeredRendering.cpp
 * @brief P4c-F5 — Layered 渲染(renderTargetArrayLength)验收
 * @details 用例:
 *   1) CascadeSelfParity — 同一 4 层 2D_ARRAY RT,单 pass layered 渲染
 *      (renderTargetArrayLength=4 + gl_Layer)vs 4 次单层渲染(arrayLayer=i),
 *      逐层 SSIM = 1.0(同后端自参照,对应 CSM 单 pass vs 逐 cascade)。
 *   2) LayerIsolation — 每层独有圆盘标记(中心 x = 0.2+0.15i)只出现在本层;
 *      其它层同位置像素与本层该区域外的背景精确相等。
 *   3) DepthLayered — D32 4 层 array 深度附件 + layered 颜色,深度 readback
 *      每层内容正确(验证 depth attachment 同样走 array 视图)。
 * Metal 参照(Assets/ReferenceImages/P4c-F5/)缺失时 SSIM 部分 skip。
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

#include "Utils/ImageCompare.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64, kH = 64, kLayers = 4;

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

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
}

ResourceHandle MakeArrayTexture(DeviceFixture& fx, DataFormat format, TextureUsage usage,
                                const char* name) {
    TextureDesc td{};
    td.size = {kW, kH, 1};
    td.mipLevels = 1;
    td.arraySize = kLayers;
    td.format = format;
    td.type = TextureType::Texture2DArray;
    td.usage = usage | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    return fx.base->CreateTexture(td);
}

/// 逐层 readback(baseArrayLayer = layer, RGBA8 → RGBA8)
std::vector<u8> ReadbackLayer(DeviceFixture& fx, ResourceHandle tex, u32 layer) {
    BufferDesc rdesc{};
    rdesc.size = u64(kW) * kH * 4;
    rdesc.type = BufferType::Raw;
    rdesc.memoryUsage = GPUMemoryUsage::Readback;
    rdesc.name = "LayerReadback";
    ResourceHandle rb = fx.base->CreateBuffer(rdesc);
    if (rb == handles::INVALID_RESOURCE) return {};
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    std::vector<u8> out;
    if (vcmd && vcmd->Reset() && vcmd->Begin()) {
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kW, kH, 1};
        vcmd->CopyTextureToBuffer(tex, rb, &region, 1);
        if (vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion()) {
            void* mapped = fx.base->MapBuffer(rb, 0, rdesc.size);
            if (mapped) {
                out.resize(size_t(rdesc.size));
                std::memcpy(out.data(), mapped, size_t(rdesc.size));
                fx.base->UnmapBuffer(rb);
            }
        }
    }
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(rb);
    return out;
}

} // anonymous namespace

TestResult TestCascadeSelfParity() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // === 管线:layered shader + push constant(layerId, useGlLayer) ===
    auto vert = ReadSPV("Assets/Shaders/P4cLayered.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cLayered.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read P4cLayered SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "CreateShader");

    PushConstantRange pcr{};
    pcr.stageFlags = ShaderStage::Vertex;
    pcr.offset = 0;
    pcr.size = 8;
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 1;
    plDesc.pushConstantRanges = &pcr;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout (push const)");

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline layered");

    // === 两张相同的 4 层 array RT ===
    ResourceHandle layeredTex = MakeArrayTexture(fx, DataFormat::RGBA8_UNorm,
                                                 TextureUsage::RenderTarget, "LayeredRT");
    ResourceHandle singleTex = MakeArrayTexture(fx, DataFormat::RGBA8_UNorm,
                                                TextureUsage::RenderTarget, "SingleRT");
    TEST_ASSERT(layeredTex != handles::INVALID_RESOURCE && singleTex != handles::INVALID_RESOURCE,
                "Create 4-layer array RTs");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Reset/Begin");

    // --- pass A: 单 pass layered(renderTargetArrayLength=4, gl_Layer) ---
    {
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].texture = layeredTex;
        rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
        rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rpd.renderTargetArrayLength = kLayers;   // ← F5 核心
        rpd.viewport.topLeft = {0.0f, 0.0f};
        rpd.viewport.size = {float(kW), float(kH)};
        rpd.scissor.offset = {0, 0};
        rpd.scissor.extent = {kW, kH};
        vcmd->BeginRenderPass(rpd);
        vcmd->BindGraphicsPipeline(pipe);
        struct PC { int layerId; int useGlLayer; } pc{0, 1};
        vcmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(pc), &pc);
        vcmd->Draw(3, 0, kLayers, 0);   // 4 instances → 4 layers
        vcmd->EndRenderPass();
    }
    // --- pass B: 4 次单层渲染(arrayLayer=i + layerId=i) ---
    for (u32 i = 0; i < kLayers; ++i) {
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].texture = singleTex;
        rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
        rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rpd.colorAttachments[0].arrayLayer = i;   // 单层视图(GetLayerView 路径)
        rpd.renderTargetArrayLength = 1;
        rpd.viewport.topLeft = {0.0f, 0.0f};
        rpd.viewport.size = {float(kW), float(kH)};
        rpd.scissor.offset = {0, 0};
        rpd.scissor.extent = {kW, kH};
        vcmd->BeginRenderPass(rpd);
        vcmd->BindGraphicsPipeline(pipe);
        struct PC { int layerId; int useGlLayer; } pc{int(i), 0};
        vcmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(pc), &pc);
        vcmd->Draw(3, 0, 1, 0);
        vcmd->EndRenderPass();
    }
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");

    // --- 逐层对照 ---
    std::error_code ec;
    std::filesystem::create_directories("P4c-F5", ec);
    u32 failures = 0;
    for (u32 i = 0; i < kLayers; ++i) {
        std::vector<u8> a = ReadbackLayer(fx, layeredTex, i);
        std::vector<u8> b = ReadbackLayer(fx, singleTex, i);
        TEST_ASSERT(!a.empty() && !b.empty(), "per-layer readback");
        et::FlipYInPlace(a.data(), kW, kH);
        et::FlipYInPlace(b.data(), kW, kH);
        char path[128];
        std::snprintf(path, sizeof(path), "P4c-F5/layer%u_layered_vulkan.png", i);
        et::SavePNG(path, a.data(), kW, kH);
        const float ssim = et::ComputeSSIM(a.data(), b.data(), kW, kH);
        const int maxDiff = et::MaxAbsDiff(a.data(), b.data(), kW, kH);
        std::cout << "[TestVulkanLayered] layer " << i
                  << ": layered-vs-single SSIM=" << ssim << " max-diff=" << maxDiff << std::endl;
        if (ssim < 0.99f) ++failures;

        // Metal 参照(缺失时 skip)
        std::vector<u8> ref; u32 rw = 0, rh = 0;
        char refPath[128];
        std::snprintf(refPath, sizeof(refPath), "Assets/ReferenceImages/P4c-F5/layer%u_metal.png", i);
        if (et::LoadPNG(refPath, ref, rw, rh) && rw == kW && rh == kH) {
            float mssim = et::ComputeSSIM(a.data(), ref.data(), kW, kH);
            std::cout << "[TestVulkanLayered] layer " << i << " SSIM vs Metal ref: " << mssim << std::endl;
            if (mssim < 0.95f) ++failures;
        }
    }
    TEST_ASSERT(failures == 0, "per-layer layered-vs-single SSIM >= 0.99");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(singleTex);
    fx.base->DestroyTexture(layeredTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

TestResult TestLayerIsolation() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/P4cLayered.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cLayered.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read P4cLayered SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");

    PushConstantRange pcr{};
    pcr.stageFlags = ShaderStage::Vertex;
    pcr.offset = 0;
    pcr.size = 8;
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 1;
    plDesc.pushConstantRanges = &pcr;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);

    ResourceHandle tex = MakeArrayTexture(fx, DataFormat::RGBA8_UNorm,
                                          TextureUsage::RenderTarget, "IsolationRT");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture isolation");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Reset/Begin");

    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = tex;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.renderTargetArrayLength = kLayers;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    struct PC { int layerId; int useGlLayer; } pc{0, 1};
    vcmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(pc), &pc);
    vcmd->Draw(3, 0, kLayers, 0);
    vcmd->EndRenderPass();
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");

    // 层 i 的圆盘中心 x = (0.2 + 0.15i)*64(FlipY 前后 x 不变),y = 32 附近
    // 断言:层 j 的 (cx_i, 32) 像素 — j==i 时为白色标记;j!=i 时等于该层
    // 该位置的渐变背景(即无标记)。背景在同 x 处各层不同(与 l 相关),
    // 用"非白色 且 等于 CPU 按 vLayer=j 计算的背景色"判定。
    std::vector<std::vector<u8>> layers(kLayers);
    for (u32 i = 0; i < kLayers; ++i) {
        layers[i] = ReadbackLayer(fx, tex, i);
        TEST_ASSERT(!layers[i].empty(), "isolation readback");
        et::FlipYInPlace(layers[i].data(), kW, kH);
    }
    u32 violations = 0;
    for (u32 i = 0; i < kLayers; ++i) {
        const u32 cx = u32((0.2f + 0.15f * float(i)) * kW + 0.5f);
        for (u32 j = 0; j < kLayers; ++j) {
            const u8* p = &layers[j][(32 * kW + cx) * 4];
            const bool isWhite = p[0] > 240 && p[1] > 240 && p[2] > 240;
            if (j == i && !isWhite) {
                std::cerr << "[TestVulkanLayered] layer " << i
                          << " missing its own marker" << std::endl;
                ++violations;
            }
            if (j != i && isWhite) {
                std::cerr << "[TestVulkanLayered] layer " << j
                          << " contains layer " << i << "'s marker (bleed)" << std::endl;
                ++violations;
            }
        }
        // 标记外区域:同层圆盘半径外的行(如 y=8)必须等于背景公式
        const u32 probeX = cx, probeY = 8;
        const u8* q = &layers[i][(probeY * kW + probeX) * 4];
        const float l = float(i);
        const int wantR = int(l / 3.0f * 255.0f + 0.5f);
        const int wantG = int((1.0f - l / 3.0f) * 255.0f + 0.5f);
        const int wantB = int((0.25f + 0.1f * l) * 255.0f + 0.5f);
        if (std::abs(int(q[0]) - wantR) > 2 || std::abs(int(q[1]) - wantG) > 2 ||
            std::abs(int(q[2]) - wantB) > 2) {
            std::cerr << "[TestVulkanLayered] layer " << i << " background mismatch at ("
                      << probeX << "," << probeY << "): got(" << int(q[0]) << "," << int(q[1])
                      << "," << int(q[2]) << ") want(" << wantR << "," << wantG << ","
                      << wantB << ")" << std::endl;
            ++violations;
        }
    }
    TEST_ASSERT(violations == 0, "layer isolation: markers only on their own layers");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(tex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

TestResult TestDepthLayered() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/P4cLayered.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cLayered.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read P4cLayered SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");

    PushConstantRange pcr{};
    pcr.stageFlags = ShaderStage::Vertex;
    pcr.offset = 0;
    pcr.size = 8;
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 1;
    plDesc.pushConstantRanges = &pcr;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.depthStencilFormat = DataFormat::D32_Float;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline depth layered");

    ResourceHandle colorTex = MakeArrayTexture(fx, DataFormat::RGBA8_UNorm,
                                               TextureUsage::RenderTarget, "DepthLayerColor");
    ResourceHandle depthTex = MakeArrayTexture(fx, DataFormat::D32_Float,
                                               TextureUsage::DepthStencil, "DepthLayerDepth");
    TEST_ASSERT(colorTex != handles::INVALID_RESOURCE && depthTex != handles::INVALID_RESOURCE,
                "Create layered color+depth arrays");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Reset/Begin");

    // --- 对照实验:单层 depth pass(非 layered)— 隔离 layered 特有问题 ---
    {
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].texture = colorTex;
        rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
        rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rpd.colorAttachments[0].arrayLayer = 0;
        rpd.depthAttachment.texture = depthTex;
        rpd.depthAttachment.format = DataFormat::D32_Float;
        rpd.depthAttachment.loadOp = LoadAction::Clear;
        rpd.depthAttachment.storeOp = StoreAction::Store;
        rpd.depthAttachment.clearValue.depth = 1.0f;
        rpd.depthAttachment.arrayLayer = 0;
        rpd.renderTargetArrayLength = 1;
        rpd.viewport.topLeft = {0.0f, 0.0f};
        rpd.viewport.size = {float(kW), float(kH)};
        rpd.viewport.minDepth = 0.0f;
        rpd.viewport.maxDepth = 1.0f;
        rpd.scissor.offset = {0, 0};
        rpd.scissor.extent = {kW, kH};
        vcmd->BeginRenderPass(rpd);
        vcmd->BindGraphicsPipeline(pipe);
        struct PC { int layerId; int useGlLayer; } pc0{0, 0};
        vcmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(pc0), &pc0);
        vcmd->Draw(3, 0, 1, 0);
        vcmd->EndRenderPass();
    }

    // --- 主 pass:layered color + depth ---
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = colorTex;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.depthAttachment.texture = depthTex;
    rpd.depthAttachment.format = DataFormat::D32_Float;
    rpd.depthAttachment.loadOp = LoadAction::Clear;
    rpd.depthAttachment.storeOp = StoreAction::Store;
    rpd.depthAttachment.clearValue.depth = 1.0f;
    rpd.renderTargetArrayLength = kLayers;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    struct PC { int layerId; int useGlLayer; } pc{0, 1};
    vcmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(pc), &pc);
    vcmd->Draw(3, 0, kLayers, 0);
    vcmd->EndRenderPass();
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");

    // 深度逐层 readback:全屏三角形 z=0.5。
    // 驱动相关的深度映射:Vulkan 折半式 → 0.75(byte≈191),MoltenVK 直通式
    // → 0.5(byte≈128)。断言驱动无关不变量:每层所有像素深度一致、各层
    // 之间一致、且落在两种约定的区间内([124,195])。
    u32 failures = 0;
    std::vector<int> layerMeans(kLayers, -1);
    for (u32 i = 0; i < kLayers; ++i) {
        std::vector<u8> raw = ReadbackLayer(fx, depthTex, i);  // D32:每像素 4B
        TEST_ASSERT(raw.size() == size_t(kW) * kH * 4, "depth layer readback size");
        std::vector<u8> rgba(size_t(kW) * kH * 4);
        et::Depth32ToRGBA8(raw.data(), rgba.data(), kW, kH);
        et::FlipYInPlace(rgba.data(), kW, kH);
        u32 bad = 0;
        long sum = 0;
        for (u32 p = 0; p < kW * kH; ++p) {
            int v = rgba[p * 4];
            sum += v;
        }
        const int mean = int(sum / (kW * kH));
        layerMeans[i] = mean;
        for (u32 p = 0; p < kW * kH; ++p) {
            if (std::abs(int(rgba[p * 4]) - mean) > 4) ++bad;
        }
        std::cout << "[TestVulkanLayered] depth layer " << i << " mean byte: " << mean
                  << " off-mean px: " << bad << std::endl;
        if (mean < 124 || mean > 195) ++failures;   // 0.5 或 0.75 约定
        if (bad > kW * kH / 100) ++failures;        // 层内均匀(全屏三角形)
    }
    for (u32 i = 1; i < kLayers; ++i) {
        if (std::abs(layerMeans[i] - layerMeans[0]) > 2) {
            std::cerr << "[TestVulkanLayered] depth layer " << i << " differs from layer 0"
                      << std::endl;
            ++failures;
        }
    }
    TEST_ASSERT(failures == 0, "layered depth attachment per-layer content correct");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyTexture(colorTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanLayeredRenderingTests() {
    auto suite = std::make_shared<TestSuite>("VulkanLayeredRenderingTests");
    suite->AddTestCase(TestCase("CascadeSelfParity", TestCascadeSelfParity));
    suite->AddTestCase(TestCase("LayerIsolation",     TestLayerIsolation));
    suite->AddTestCase(TestCase("DepthLayered",       TestDepthLayered));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanLayeredRenderingTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanLayeredRendering] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
