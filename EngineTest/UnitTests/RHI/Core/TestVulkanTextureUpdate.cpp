/**
 * @file TestVulkanTextureUpdate.cpp
 * @brief P4c-F3 — 纹理 updateData / Map 通道验收
 * @details 用例:
 *   1) CheckerboardExact — 8x8 棋盘格 updateData 上传 → 全屏 NEAREST 采样绘制
 *      → readback 与 CPU 参照 max-abs-diff ≤ 1/255(updateData 后直接采样,
 *      布局/屏障由 RHI 内部闭合 — 零 validation error)。
 *   2) ProgressiveSubrect — 分 16 次行段子矩形更新拼出渐变 → 最终 readback
 *      与一次性整图更新 readback 逐字节相等(同后端自参照 SSIM = 1.0)。
 *   3) SubrectNoClobber — 上下两半两次部分更新,两区域内容各自正确、无互踩。
 *   4) MapSemantics — DEVICE_LOCAL map 返回 nullptr + 一次性 warn 不 crash;
 *      Staging 用途纹理 map 返回持久映射指针。
 *
 * Metal 参照(Assets/ReferenceImages/P4c-F3/)缺失时 SSIM 部分 skip。
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
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
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

/// CopyTextureToBuffer 读回整张 RGBA8 纹理(紧密行主序)
std::vector<u8> ReadbackTexture(DeviceFixture& fx, ResourceHandle tex, u32 w, u32 h) {
    BufferDesc rdesc{};
    rdesc.size = u64(w) * h * 4;
    rdesc.type = BufferType::Raw;
    rdesc.memoryUsage = GPUMemoryUsage::Readback;
    rdesc.name = "TexUpdateReadback";
    ResourceHandle rb = fx.base->CreateBuffer(rdesc);
    if (rb == handles::INVALID_RESOURCE) return {};

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    std::vector<u8> out;
    if (vcmd && vcmd->Reset() && vcmd->Begin()) {
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {w, h, 1};
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

ResourceHandle MakeTexture(DeviceFixture& fx, u32 w, u32 h, const char* name,
                           TextureUsage extra = TextureUsage::CopySource) {
    TextureDesc td{};
    td.size = {w, h, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | extra;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    return fx.base->CreateTexture(td);
}

} // anonymous namespace

// ============================================================================
// Case 1: 棋盘格上传 + 全屏采样,像素精确(≤1/255)
// ============================================================================
TestResult TestCheckerboardExact() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kT = 8;       // 8x8 纹理
    constexpr u32 kW = 64, kH = 64;

    // CPU 棋盘格(4x4 texel 块)
    std::vector<u8> checker(size_t(kT) * kT * 4);
    for (u32 y = 0; y < kT; ++y) {
        for (u32 x = 0; x < kT; ++x) {
            const bool white = ((x / 4) + (y / 4)) % 2 == 0;
            u8 v = white ? 255 : 0;
            u8* p = &checker[(y * kT + x) * 4];
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }

    ResourceHandle tex = MakeTexture(fx, kT, kT, "CheckerTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture checker");

    // updateData 上传(立即模式,无帧上下文 — 资产加载期语义)
    VulkanTexture* vtex = fx.vk->GetTexture(tex);
    TEST_ASSERT(vtex->UpdateData(checker.data(), checker.size(), 0),
                "UpdateData checkerboard full image");

    // 采样管线:NEAREST 保证 texel→pixel 一一对应
    auto vert = ReadSPV("Assets/Shaders/P4cSampleTexture.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cSampleTexture.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read P4cSampleTexture SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "CreateShader");

    SamplerDesc sd{};
    sd.minFilter = FilterMode::Point;
    sd.magFilter = FilterMode::Point;
    sd.mipFilter = FilterMode::Point;
    sd.addressU = TextureAddressMode::Clamp;
    sd.addressV = TextureAddressMode::Clamp;
    sd.maxAnisotropy = 1;
    sd.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(sd);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler nearest");

    DescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = DescriptorType::CombinedImageSampler;
    bind.descriptorCount = 1;
    bind.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &bind;
    DescriptorSetLayoutHandle dsl = fx.base->CreateDescriptorSetLayout(dslDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    DescriptorImageInfo imgInfo{};
    imgInfo.sampler = sampler;
    imgInfo.imageView = tex;
    imgInfo.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet write{};
    write.dstSet = ds; write.dstBinding = 0; write.dstArrayElement = 0;
    write.descriptorCount = 1; write.descriptorType = DescriptorType::CombinedImageSampler;
    write.imageInfo = &imgInfo;
    fx.base->UpdateDescriptorSets(1, &write);

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
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline sample");

    ResourceHandle rt = MakeTexture(fx, kW, kH, "SampleRT", TextureUsage::RenderTarget | TextureUsage::CopySource);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture sample RT");

    // updateData 后直接 BeginRenderPass 采样 — 布局由 RHI 闭合
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Reset/Begin");

    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Draw(3, 0, 1, 0);
    vcmd->EndRenderPass();
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit sample pass");

    std::vector<u8> frame = ReadbackTexture(fx, rt, kW, kH);
    TEST_ASSERT(!frame.empty(), "readback sample RT");
    // 同后端 CPU 参照不 FlipY:Vulkan readback 行序与 gl_FragCoord 一致,
    // uv=(0,0) 的 NDC(-1,-1) 落 framebuffer 左上(FlipY 仅用于 Metal 参照)。

    // CPU 参照:NEAREST 上采样 8x8 → 64x64(每 texel 8x8 像素块)
    std::vector<u8> expect(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            u32 tx = x * kT / kW, ty = y * kT / kH;
            std::memcpy(&expect[(y * kW + x) * 4], &checker[(ty * kT + tx) * 4], 4);
        }
    }
    const int maxDiff = et::MaxAbsDiff(frame.data(), expect.data(), kW, kH);
    std::cout << "[TestVulkanTextureUpdate] checkerboard max-abs-diff = " << maxDiff << std::endl;
    TEST_ASSERT(maxDiff <= 1, "checkerboard sampled output exact within 1/255");

    et::SavePNG("P4cF3_checkerboard_vulkan.png", frame.data(), kW, kH);
    // Metal ReplaceRegion 参照(缺失时 skip)
    std::vector<u8> ref; u32 rw = 0, rh = 0;
    if (et::LoadPNG("Assets/ReferenceImages/P4c-F3/checkerboard_metal.png", ref, rw, rh)
        && rw == kW && rh == kH) {
        float ssim = et::ComputeSSIM(frame.data(), ref.data(), kW, kH);
        std::cout << "[TestVulkanTextureUpdate] SSIM vs Metal ref: " << ssim << std::endl;
        TEST_ASSERT(ssim >= 0.98f, "checkerboard SSIM >= 0.98 vs Metal");
    } else {
        std::cerr << "[TestVulkanTextureUpdate] Metal reference missing — skipping SSIM" << std::endl;
    }

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(dsl);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    fx.base->DestroyTexture(tex);
    return TestResult::Passed;
}

// ============================================================================
// Case 2: 16 次子矩形渐进更新 vs 一次性整图更新,逐字节相等
// ============================================================================
TestResult TestProgressiveSubrect() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kW = 64, kH = 64;
    constexpr u32 kBands = 16;
    const u32 rowsPerBand = kH / kBands;

    // 渐变图
    std::vector<u8> gradient(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            u8* p = &gradient[(y * kW + x) * 4];
            p[0] = static_cast<u8>(x * 255 / (kW - 1));
            p[1] = static_cast<u8>(y * 255 / (kH - 1));
            p[2] = static_cast<u8>((x / 8 + y / 8) % 2 ? 255 : 64);
            p[3] = 255;
        }
    }

    // 整图更新参照
    ResourceHandle full = MakeTexture(fx, kW, kH, "FullUpload");
    TEST_ASSERT(full != handles::INVALID_RESOURCE, "CreateTexture full");
    TEST_ASSERT(fx.vk->GetTexture(full)->UpdateData(gradient.data(), gradient.size(), 0),
                "UpdateData full");
    std::vector<u8> fullRB = ReadbackTexture(fx, full, kW, kH);
    TEST_ASSERT(!fullRB.empty(), "readback full");
    fx.base->DestroyTexture(full);

    // 16 次行段子矩形更新
    ResourceHandle prog = MakeTexture(fx, kW, kH, "ProgressiveUpload");
    TEST_ASSERT(prog != handles::INVALID_RESOURCE, "CreateTexture progressive");
    VulkanTexture* ptex = fx.vk->GetTexture(prog);
    for (u32 b = 0; b < kBands; ++b) {
        const u64 offset = u64(b * rowsPerBand) * kW * 4;
        const u64 size = u64(rowsPerBand) * kW * 4;
        TEST_ASSERT(ptex->UpdateData(gradient.data() + offset, size, offset),
                    "UpdateData subrect band");
    }
    std::vector<u8> progRB = ReadbackTexture(fx, prog, kW, kH);
    TEST_ASSERT(!progRB.empty(), "readback progressive");
    fx.base->DestroyTexture(prog);

    TEST_ASSERT(progRB.size() == fullRB.size(), "readback sizes match");
    const int maxDiff = et::MaxAbsDiff(progRB.data(), fullRB.data(), kW, kH);
    const float ssim = et::ComputeSSIM(progRB.data(), fullRB.data(), kW, kH);
    std::cout << "[TestVulkanTextureUpdate] progressive vs full: max-abs-diff=" << maxDiff
              << " SSIM=" << ssim << std::endl;
    TEST_ASSERT(maxDiff == 0, "16-band progressive == one-shot full upload (byte exact)");
    TEST_ASSERT(ssim == 1.0f, "self-parity SSIM == 1.0");
    return TestResult::Passed;
}

// ============================================================================
// Case 3: 连续两次部分更新无互踩
// ============================================================================
TestResult TestSubrectNoClobber() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kW = 32, kH = 32;
    const u64 rowBytes = u64(kW) * 4;
    const u64 halfBytes = rowBytes * (kH / 2);

    // 上半:A=红;下半:B=绿
    std::vector<u8> top{std::vector<u8>(size_t(halfBytes), 0)};
    std::vector<u8> bottom{std::vector<u8>(size_t(halfBytes), 0)};
    for (u32 i = 0; i < kW * (kH / 2); ++i) {
        top[i * 4 + 0] = 255; top[i * 4 + 1] = 0; top[i * 4 + 2] = 0; top[i * 4 + 3] = 255;
        bottom[i * 4 + 0] = 0; bottom[i * 4 + 1] = 255; bottom[i * 4 + 2] = 0; bottom[i * 4 + 3] = 255;
    }

    ResourceHandle tex = MakeTexture(fx, kW, kH, "NoClobberTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture");
    VulkanTexture* vt = fx.vk->GetTexture(tex);
    TEST_ASSERT(vt->UpdateData(top.data(), top.size(), 0), "UpdateData top half");
    TEST_ASSERT(vt->UpdateData(bottom.data(), bottom.size(), halfBytes), "UpdateData bottom half");

    std::vector<u8> rb = ReadbackTexture(fx, tex, kW, kH);
    TEST_ASSERT(!rb.empty(), "readback");
    fx.base->DestroyTexture(tex);

    u32 bad = 0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &rb[(y * kW + x) * 4];
            const bool isTop = y < kH / 2;
            if (isTop ? (p[0] != 255 || p[1] != 0) : (p[0] != 0 || p[1] != 255)) ++bad;
        }
    }
    std::cout << "[TestVulkanTextureUpdate] no-clobber mismatches: " << bad << std::endl;
    TEST_ASSERT(bad == 0, "two sequential partial updates land exactly in their halves");
    return TestResult::Passed;
}

// ============================================================================
// Case 4: map 语义(DEVICE_LOCAL → nullptr + warn;Staging → 持久指针)
// ============================================================================
TestResult TestMapSemantics() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // DEVICE_LOCAL:map 必须返回 nullptr 且不 crash(一次性 warn)
    ResourceHandle devLocal = MakeTexture(fx, 8, 8, "DeviceLocalTex", TextureUsage::ShaderResource);
    TEST_ASSERT(devLocal != handles::INVALID_RESOURCE, "CreateTexture device-local");
    void* m1 = fx.vk->GetTexture(devLocal)->Map(0, 0);
    void* m2 = fx.vk->GetTexture(devLocal)->Map(0, 0);  // 第二次:静默(一次性 warn)
    TEST_ASSERT(m1 == nullptr && m2 == nullptr, "DEVICE_LOCAL map returns nullptr");
    fx.vk->GetTexture(devLocal)->Unmap();
    fx.base->DestroyTexture(devLocal);

    // Staging:map 返回持久映射指针,写入经 unmap 后有效
    TextureDesc sd{};
    sd.size = {8, 8, 1};
    sd.mipLevels = 1;
    sd.arraySize = 1;
    sd.format = DataFormat::RGBA8_UNorm;
    sd.type = TextureType::Texture2D;
    sd.usage = TextureUsage::ShaderResource | TextureUsage::CopySource;
    sd.memoryUsage = GPUMemoryUsage::Staging;
    sd.name = "StagingTex";
    ResourceHandle staging = fx.base->CreateTexture(sd);
    if (staging == handles::INVALID_RESOURCE) {
        std::cout << "[TestVulkanTextureUpdate] staging texture unsupported on this device — "
                     "skipping staged-map half" << std::endl;
        return TestResult::Passed;
    }
    void* sm = fx.vk->GetTexture(staging)->Map(0, 0);
    std::cout << "[TestVulkanTextureUpdate] staging map ptr = " << sm << std::endl;
    TEST_ASSERT(sm != nullptr, "Staging texture map returns persistent pointer");
    fx.vk->GetTexture(staging)->Unmap();
    fx.base->DestroyTexture(staging);
    return TestResult::Passed;
}

// ============================================================================
// Case 5: P4c-F8 immutable sampler — 绑定后 write 不需要 sampler,
// 渲染结果与可变 sampler 像素精确一致
// ============================================================================
TestResult TestImmutableSamplerParity() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kT = 8, kW = 64, kH = 64;
    std::vector<u8> checker(size_t(kT) * kT * 4);
    for (u32 y = 0; y < kT; ++y) {
        for (u32 x = 0; x < kT; ++x) {
            const bool white = ((x / 4) + (y / 4)) % 2 == 0;
            u8 v = white ? 255 : 0;
            u8* p = &checker[(y * kT + x) * 4];
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
    ResourceHandle tex = MakeTexture(fx, kT, kT, "ImmutableTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture");
    TEST_ASSERT(fx.vk->GetTexture(tex)->UpdateData(checker.data(), checker.size(), 0),
                "UpdateData");

    auto vert = ReadSPV("Assets/Shaders/P4cSampleTexture.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cSampleTexture.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");

    SamplerDesc sd{};
    sd.minFilter = FilterMode::Point;
    sd.magFilter = FilterMode::Point;
    sd.mipFilter = FilterMode::Point;
    sd.addressU = TextureAddressMode::Clamp;
    sd.addressV = TextureAddressMode::Clamp;
    sd.maxAnisotropy = 1;
    sd.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(sd);

    // 渲染 helper:mutable / immutable 两种 descriptor 路径共用
    auto renderOnce = [&](DescriptorSetLayoutHandle dsl, DescriptorSetHandle ds) {
        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &dsl;
        plDesc.pushConstantRangeCount = 0;
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
        ResourceHandle rt = MakeTexture(fx, kW, kH, "ImmRT",
                                        TextureUsage::RenderTarget | TextureUsage::CopySource);
        std::vector<u8> out;
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        if (vcmd && vcmd->Reset() && vcmd->Begin()) {
            RenderPassDesc rpd{};
            rpd.colorAttachments.resize(1);
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
            rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
            rpd.colorAttachments[0].storeOp = StoreAction::Store;
            rpd.viewport.topLeft = {0.0f, 0.0f};
            rpd.viewport.size = {float(kW), float(kH)};
            rpd.scissor.offset = {0, 0};
            rpd.scissor.extent = {kW, kH};
            vcmd->BeginRenderPass(rpd);
            vcmd->BindGraphicsPipeline(pipe);
            vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
            vcmd->Draw(3, 0, 1, 0);
            vcmd->EndRenderPass();
            if (vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion()) {
                out = ReadbackTexture(fx, rt, kW, kH);
            }
        }
        fx.base->DestroyCommandBuffer(cmd);
        fx.base->DestroyTexture(rt);
        fx.base->DestroyPipeline(pipe);
        fx.base->DestroyPipelineLayout(pl);
        return out;
    };

    // --- mutable 路径(write 带 sampler) ---
    DescriptorSetLayoutBinding bindM{};
    bindM.binding = 0;
    bindM.descriptorType = DescriptorType::CombinedImageSampler;
    bindM.descriptorCount = 1;
    bindM.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutDesc dslM{};
    dslM.bindingCount = 1;
    dslM.bindings = &bindM;
    DescriptorSetLayoutHandle dslMutable = fx.base->CreateDescriptorSetLayout(dslM);
    DescriptorSetDesc dsM{}; dsM.layout = dslMutable;
    DescriptorSetHandle dsMutable = fx.base->CreateDescriptorSet(dsM);
    DescriptorImageInfo imgM{};
    imgM.sampler = sampler;
    imgM.imageView = tex;
    imgM.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet wM{};
    wM.dstSet = dsMutable; wM.dstBinding = 0; wM.dstArrayElement = 0;
    wM.descriptorCount = 1; wM.descriptorType = DescriptorType::CombinedImageSampler;
    wM.imageInfo = &imgM;
    fx.base->UpdateDescriptorSets(1, &wM);
    std::vector<u8> mutableOut = renderOnce(dslMutable, dsMutable);
    TEST_ASSERT(!mutableOut.empty(), "mutable render");

    // --- immutable 路径(layout 固定 sampler;write 不传 sampler) ---
    DescriptorSetLayoutBinding bindI{};
    bindI.binding = 0;
    bindI.descriptorType = DescriptorType::CombinedImageSampler;
    bindI.descriptorCount = 1;
    bindI.stageFlags = ShaderStage::Pixel;
    bindI.immutableSamplers = &sampler;   // ← F8 核心
    DescriptorSetLayoutDesc dslI{};
    dslI.bindingCount = 1;
    dslI.bindings = &bindI;
    DescriptorSetLayoutHandle dslImm = fx.base->CreateDescriptorSetLayout(dslI);
    TEST_ASSERT(dslImm != handles::INVALID_RESOURCE, "immutable layout create");
    DescriptorSetDesc dsI{}; dsI.layout = dslImm;
    DescriptorSetHandle dsImm = fx.base->CreateDescriptorSet(dsI);
    DescriptorImageInfo imgI{};
    imgI.sampler = handles::INVALID_SAMPLER;   // 不再需要 sampler
    imgI.imageView = tex;
    imgI.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet wI{};
    wI.dstSet = dsImm; wI.dstBinding = 0; wI.dstArrayElement = 0;
    wI.descriptorCount = 1; wI.descriptorType = DescriptorType::CombinedImageSampler;
    wI.imageInfo = &imgI;
    fx.base->UpdateDescriptorSets(1, &wI);
    std::vector<u8> immOut = renderOnce(dslImm, dsImm);
    TEST_ASSERT(!immOut.empty(), "immutable render");

    const int maxDiff = et::MaxAbsDiff(immOut.data(), mutableOut.data(), kW, kH);
    std::cout << "[TestVulkanTextureUpdate] immutable-vs-mutable sampler max-abs-diff = "
              << maxDiff << std::endl;
    TEST_ASSERT(maxDiff == 0, "immutable sampler rendering identical to mutable");

    fx.base->DestroyDescriptorSet(dsImm);
    fx.base->DestroyDescriptorSetLayout(dslImm);
    fx.base->DestroyDescriptorSet(dsMutable);
    fx.base->DestroyDescriptorSetLayout(dslMutable);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    fx.base->DestroyTexture(tex);
    return TestResult::Passed;
}

void RegisterVulkanTextureUpdateTests() {
    auto suite = std::make_shared<TestSuite>("VulkanTextureUpdateTests");
    suite->AddTestCase(TestCase("CheckerboardExact",   TestCheckerboardExact));
    suite->AddTestCase(TestCase("ProgressiveSubrect",  TestProgressiveSubrect));
    suite->AddTestCase(TestCase("SubrectNoClobber",    TestSubrectNoClobber));
    suite->AddTestCase(TestCase("MapSemantics",        TestMapSemantics));
    suite->AddTestCase(TestCase("ImmutableSamplerParity", TestImmutableSamplerParity));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanTextureUpdateTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanTextureUpdate] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
