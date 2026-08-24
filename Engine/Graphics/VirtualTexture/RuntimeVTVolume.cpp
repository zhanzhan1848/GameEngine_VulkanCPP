/**
 * @file RuntimeVTVolume.cpp
 * @brief VT M5 — RVT Volume 实现(三 pass 页生产)
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-23
 */

#include "RuntimeVTVolume.h"
#include "../RHI/Core/RHIDevice.h"

#include <fstream>
#include <iostream>

namespace primal::graphics::vt {

using namespace primal::graphics::rhi;

namespace {
std::vector<char> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<char> d(size_t(sz), 0);
    f.read(d.data(), sz);
    return d;
}
} // anonymous namespace

bool runtime_vt_volume::initialize(RHIDeviceBase* device, const desc& d) {
    device_ = device;
    desc_ = d;
    return build_pipelines(device) && ensure_targets(device);
}

void runtime_vt_volume::shutdown() {
    if (!device_) return;
    if (capture_rt_) device_->DestroyTexture(capture_rt_);
    if (packed_tex_) device_->DestroyTexture(packed_tex_);
    if (capture_pipeline_) device_->DestroyPipeline(capture_pipeline_);
    if (compress_pipeline_) device_->DestroyPipeline(compress_pipeline_);
    if (finalize_pipeline_) device_->DestroyPipeline(finalize_pipeline_);
    if (capture_layout_) device_->DestroyPipelineLayout(capture_layout_);
    if (compress_layout_) device_->DestroyPipelineLayout(compress_layout_);
    if (finalize_layout_) device_->DestroyPipelineLayout(finalize_layout_);
    if (compress_ds_) device_->DestroyDescriptorSet(compress_ds_);
    if (finalize_ds_) device_->DestroyDescriptorSet(finalize_ds_);
    if (compress_ds_layout_) device_->DestroyDescriptorSetLayout(compress_ds_layout_);
    if (finalize_ds_layout_) device_->DestroyDescriptorSetLayout(finalize_ds_layout_);
    if (sampler_) device_->DestroySampler(sampler_);
    if (capture_vs_) device_->DestroyShader(capture_vs_);
    if (capture_fs_) device_->DestroyShader(capture_fs_);
    if (compress_cs_) device_->DestroyShader(compress_cs_);
    if (finalize_cs_) device_->DestroyShader(finalize_cs_);
    capture_rt_ = packed_tex_ = 0;
    capture_pipeline_ = compress_pipeline_ = finalize_pipeline_ = 0;
    capture_layout_ = compress_layout_ = finalize_layout_ = 0;
    compress_ds_ = finalize_ds_ = 0;
    compress_ds_layout_ = finalize_ds_layout_ = 0;
    sampler_ = 0;
    capture_vs_ = capture_fs_ = compress_cs_ = finalize_cs_ = 0;
    device_ = nullptr;
}

bool runtime_vt_volume::build_pipelines(RHIDeviceBase* device) {
    auto vsBytes = readSpv("Assets/Shaders/RVTCapture.vert.spv");
    auto fsBytes = readSpv("Assets/Shaders/RVTCapture.frag.spv");
    auto compressBytes = readSpv("Assets/Shaders/RVTCompress.comp.spv");
    auto finalizeBytes = readSpv("Assets/Shaders/RVTFinalize.comp.spv");
    if (vsBytes.empty() || fsBytes.empty() || compressBytes.empty() || finalizeBytes.empty()) {
        std::cerr << "[RVT] 缺少 RVTCapture/RVTCompress/RVTFinalize spv" << std::endl;
        return false;
    }
    capture_vs_ = device->CreateShader(vsBytes.data(), vsBytes.size(), ShaderStage::Vertex, "main");
    capture_fs_ = device->CreateShader(fsBytes.data(), fsBytes.size(), ShaderStage::Pixel, "main");
    compress_cs_ = device->CreateShader(compressBytes.data(), compressBytes.size(), ShaderStage::Compute, "main");
    finalize_cs_ = device->CreateShader(finalizeBytes.data(), finalizeBytes.size(), ShaderStage::Compute, "main");
    if (capture_vs_ == handles::INVALID_SHADER || capture_fs_ == handles::INVALID_SHADER ||
        compress_cs_ == handles::INVALID_SHADER || finalize_cs_ == handles::INVALID_SHADER)
        return false;

    // ---- 捕获管线(全屏三角;push 常量带世界矩形)----
    {
        PipelineLayoutDesc pl{};
        pl.setLayoutCount = 0;
        pl.setLayouts = nullptr;
        PushConstantRange pcr{ShaderStage::Pixel, 0, 16};
        pl.pushConstantRangeCount = 1;
        pl.pushConstantRanges = &pcr;
        capture_layout_ = device->CreatePipelineLayout(pl);

        GraphicsPipelineDesc gpd{};
        gpd.layout = capture_layout_;
        gpd.vertexShader = capture_vs_;
        gpd.pixelShader = capture_fs_;
        gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
        gpd.renderTargetCount = 1;
        gpd.depthStencilFormat = DataFormat::Unknown;
        gpd.enableDepthTest = false;
        gpd.enableDepthWrite = false;
        gpd.cullMode = CullMode::None;
        capture_pipeline_ = device->CreateGraphicsPipeline(gpd);
        if (capture_pipeline_ == handles::INVALID_PIPELINE) {
        std::cerr << "[RVT] capture pipeline failed" << std::endl;
        return false;
    }
    }

    SamplerDesc sd{};
    sd.minFilter = FilterMode::Nearest;
    sd.magFilter = FilterMode::Nearest;
    sd.addressU = TextureAddressMode::Clamp;
    sd.addressV = TextureAddressMode::Clamp;
    sampler_ = device->CreateSampler(sd);

    // ---- 压缩 CS:capture(采样)+ packed(存储镜像) + params UBO ----
    {
        DescriptorSetLayoutBinding b[3]{};
        b[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        b[1] = {1, DescriptorType::Sampler, 1, ShaderStage::Compute, nullptr};
        b[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        DescriptorSetLayoutDesc dsl{3, b};
        compress_ds_layout_ = device->CreateDescriptorSetLayout(dsl);
        PipelineLayoutDesc pl{};
        pl.setLayoutCount = 1;
        pl.setLayouts = &compress_ds_layout_;
        pl.pushConstantRangeCount = 1;
        PushConstantRange pcr{ShaderStage::Compute, 0, 16};
        pl.pushConstantRanges = &pcr;
        compress_layout_ = device->CreatePipelineLayout(pl);
        ComputePipelineDesc cpd{};
        cpd.computeShader = compress_cs_;
        cpd.layout = compress_layout_;
        cpd.threadGroupSize = {8, 8, 1};
        compress_pipeline_ = device->CreateComputePipeline(cpd);
        DescriptorSetDesc dd{};
        dd.layout = compress_ds_layout_;
        compress_ds_ = device->CreateDescriptorSet(dd);
    }

    // ---- Finalize CS:packed(只读存储镜像)+ atlas(存储镜像) ----
    {
        DescriptorSetLayoutBinding b[2]{};
        b[0] = {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        b[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        DescriptorSetLayoutDesc dsl{2, b};
        finalize_ds_layout_ = device->CreateDescriptorSetLayout(dsl);
        PipelineLayoutDesc pl{};
        pl.setLayoutCount = 1;
        pl.setLayouts = &finalize_ds_layout_;
        pl.pushConstantRangeCount = 1;
        PushConstantRange pcr{ShaderStage::Compute, 0, 16};
        pl.pushConstantRanges = &pcr;
        finalize_layout_ = device->CreatePipelineLayout(pl);
        ComputePipelineDesc cpd{};
        cpd.computeShader = finalize_cs_;
        cpd.layout = finalize_layout_;
        cpd.threadGroupSize = {8, 8, 1};
        finalize_pipeline_ = device->CreateComputePipeline(cpd);
        DescriptorSetDesc dd{};
        dd.layout = finalize_ds_layout_;
        finalize_ds_ = device->CreateDescriptorSet(dd);
    }
    if (compress_pipeline_ == handles::INVALID_PIPELINE)
        std::cerr << "[RVT] compress pipeline failed" << std::endl;
    if (finalize_pipeline_ == handles::INVALID_PIPELINE)
        std::cerr << "[RVT] finalize pipeline failed" << std::endl;
    return capture_pipeline_ != handles::INVALID_PIPELINE &&
           compress_pipeline_ != handles::INVALID_PIPELINE &&
           finalize_pipeline_ != handles::INVALID_PIPELINE;
}

bool runtime_vt_volume::ensure_targets(RHIDeviceBase* device) {
    const u32 phys = desc_.tile_size + 2 * desc_.border;
    TextureDesc rtDesc{};
    rtDesc.size = {phys, phys, 1};
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA8_UNorm;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "RVT_CaptureRT";
    capture_rt_ = device->CreateTexture(rtDesc);

    TextureDesc pkDesc = rtDesc;
    pkDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
    pkDesc.name = "RVT_Packed";
    packed_tex_ = device->CreateTexture(pkDesc);
    return capture_rt_ != handles::INVALID_RESOURCE &&
           packed_tex_ != handles::INVALID_RESOURCE;
}

void runtime_vt_volume::page_world_rect(u32 px, u32 py, u32 mip, u32 pages_top,
                                        float& wx0, float& wz0, float& wx1, float& wz1) const {
    // mip 页格嵌套于子矩形左上(金字塔打包语义);页含 border 外扩(图外 clamp)
    const u32 s = std::max(1u, pages_top >> mip);
    const float pageW = (desc_.world_x1 - desc_.world_x0) / float(s);
    const float pageH = (desc_.world_z1 - desc_.world_z0) / float(s);
    // border 世界外扩:border/tile 占页的比例 × 页宽(量纲 = 世界单位)
    const float bx = (float(desc_.border) / float(desc_.tile_size)) * pageW;
    const float by = (float(desc_.border) / float(desc_.tile_size)) * pageH;
    wx0 = desc_.world_x0 + pageW * float(px) - bx;
    wz0 = desc_.world_z0 + pageH * float(py) - by;
    wx1 = desc_.world_x0 + pageW * float(px + 1) + bx;
    wz1 = desc_.world_z0 + pageH * float(py + 1) + by;
}

bool runtime_vt_volume::produce_page(RHIDeviceBase* device, u32 vt_id, u32 page,
                                     u32 layer, ResourceHandle atlas,
                                     u32 atlas_x, u32 atlas_y) {
    (void)vt_id;
    (void)layer;   // v1 单层
    auto& sys = virtual_texture_system::instance();
    const u32 pages_top = sys.vt_pages_top(desc_.vt_id);
    if (pages_top == 0) return false;
    // 线性页 → (mip, px, py)(金字塔打包)
    u32 base = 0, s = pages_top, mip = 0;
    while (base + s * s <= page) { base += s * s; s >>= 1; ++mip; }
    const u32 px = (page - base) % s;
    const u32 py = (page - base) / s;

    float wx0, wz0, wx1, wz1;
    page_world_rect(px, py, mip, pages_top, wx0, wz0, wx1, wz1);

    const u32 phys = desc_.tile_size + 2 * desc_.border;
    CommandBufferHandle cmd = device->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
    if (!cb) return false;
    cb->Reset();
    cb->Begin();

    // ---- Pass 1:捕获(Draw 全屏三角 → capture RT)----
    RenderPassDesc rp{};
    rp.colorAttachments.resize(1);
    rp.colorAttachments[0].texture = capture_rt_;
    rp.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rp.colorAttachments[0].loadOp = LoadAction::DontCare;
    rp.colorAttachments[0].storeOp = StoreAction::Store;
    rp.viewport.size = {float(phys), float(phys)};
    rp.scissor.extent = {phys, phys};
    cb->BeginRenderPass(rp);
    cb->BindGraphicsPipeline(capture_pipeline_);
    struct Push { float r[4]; } push{{wx0, wz0, wx1, wz1}};
    cb->PushConstants(capture_layout_, ShaderStage::Pixel, 0, sizeof(Push), &push);
    cb->Draw(3, 0, 1, 0);
    cb->EndRenderPass();

    // ---- Pass 2:压缩(capture 采样 → packed 存储镜像;BCn 钩子位)----
    {
        DescriptorImageInfo capInfo{}, sampInfo{}, pkInfo{};
        capInfo.imageView = capture_rt_;
        capInfo.imageLayout = ResourceState::ShaderResource;
        sampInfo.sampler = sampler_;
        pkInfo.imageView = packed_tex_;
        pkInfo.imageLayout = ResourceState::UnorderedAccess;
        WriteDescriptorSet w[3]{};
        for (u32 i = 0; i < 3; ++i) {
            w[i].dstSet = compress_ds_; w[i].dstBinding = i; w[i].dstArrayElement = 0;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = DescriptorType::SampledImage; w[0].imageInfo = &capInfo;
        w[1].descriptorType = DescriptorType::Sampler;      w[1].imageInfo = &sampInfo;
        w[2].descriptorType = DescriptorType::StorageImage; w[2].imageInfo = &pkInfo;
        device->UpdateDescriptorSets(3, w);
    }
    ResourceBarrier b1{};
    b1.resource = capture_rt_;
    b1.beforeState = ResourceState::RenderTarget;
    b1.afterState = ResourceState::ShaderResource;
    b1.subresource = 0xFFFFFFFF;
    b1.queueFamily = 0xFFFFFFFF;
    cb->InsertBarrier(&b1, 1);
    {
        struct P { u32 o[2]; u32 t[2]; } p{{atlas_x, atlas_y}, {phys, phys}};
        cb->BindComputePipeline(compress_pipeline_);
        cb->BindDescriptorSets(PipelineBindPoint::Compute, compress_layout_, 0, 1,
                               &compress_ds_, 0, nullptr);
        cb->PushConstants(compress_layout_, ShaderStage::Compute, 0, sizeof(P), &p);
        cb->Dispatch((phys + 7) / 8, (phys + 7) / 8, 1);
    }

    // ---- Pass 3:GPUFinalizer(packed → atlas 存储镜像直写)----
    {
        DescriptorImageInfo pkInfo{}, atInfo{};
        pkInfo.imageView = packed_tex_;
        pkInfo.imageLayout = ResourceState::UnorderedAccess;
        atInfo.imageView = atlas;
        atInfo.imageLayout = ResourceState::UnorderedAccess;
        WriteDescriptorSet w[2]{};
        w[0].dstSet = finalize_ds_; w[0].dstBinding = 0; w[0].dstArrayElement = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = DescriptorType::StorageImage;
        w[0].imageInfo = &pkInfo;
        w[1].dstSet = finalize_ds_; w[1].dstBinding = 1; w[1].dstArrayElement = 0;
        w[1].descriptorCount = 1;
        w[1].descriptorType = DescriptorType::StorageImage;
        w[1].imageInfo = &atInfo;
        device->UpdateDescriptorSets(2, w);
    }
    cb->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                      AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
    {
        struct P { u32 o[2]; u32 t[2]; } p{{atlas_x, atlas_y}, {phys, phys}};
        cb->BindComputePipeline(finalize_pipeline_);
        cb->BindDescriptorSets(PipelineBindPoint::Compute, finalize_layout_, 0, 1,
                               &finalize_ds_, 0, nullptr);
        cb->PushConstants(finalize_layout_, ShaderStage::Compute, 0, sizeof(P), &p);
        cb->Dispatch((phys + 7) / 8, (phys + 7) / 8, 1);
    }
    cb->End();
    cb->Submit(0);
    cb->WaitForCompletion();   // M5 v1:同步;M6 批化
    device->DestroyCommandBuffer(cmd);

    // capture RT 布局复位(下次捕获前 render pass 自行转换;RHI 跟踪已更新)
    ++produced_pages_;
    return true;
}

} // namespace primal::graphics::vt
