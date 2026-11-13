/**
 * @file TestVulkanVTPrototype.cpp
 * @brief VT M0.9-0.12 — 端到端原型(Vulkan/MoltenVK):页表 + 物理 atlas + 两段式采样。
 * @details 无 feedback 的 CPU 驱动流送闭环,验证 VT 采样地址翻译全链路:
 *
 *   1. CPU 生成 1024² 源图 + box-filter mip 链(1024/512/256/128)(M0.9 数据布局)
 *   2. tile 128 + border 4(clamp 扩展)上传进 2048² 物理 atlas
 *      (UpdateTextureRegion 局部上传 — M0.7 在 VT 布局下的运用)
 *   3. 页表 16x16 R32_UINT(单 mip 打包布局,mip0/1 层级占不同列区间),
 *      由 CS 从 entries buffer 批量写入(M0.12: imageStore + 屏障一致性,
 *      同一 cmdbuf 内 写→采样 两轮,跨 cmdbuf 更新可见性)
 *   4. 采样 CS:uv → 页表 texelFetch 解码 atlas origin → atlas texelFetch,
 *      texel 中心整数取数 → 与 CPU 参考逐像素精确比对(M0.10/M0.11)
 *   5. 第二"帧":补丁 tile 上传到新 atlas 槽位 + 跨 cmdbuf 页表项更新 +
 *      重采样,验证页表更新对后续帧可见(M0.12 跨帧一致性)
 *
 * 页表条目位布局(R32_UINT): bit0=valid, bits[8:19)=atlasOriginX,
 * bits[19:30)=atlasOriginY — 与 VTProto_*.comp 保持一致。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

using namespace primal;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

namespace {

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

// ============================= 原型常量(与 shader 一致) =============================
constexpr u32 kSrcSize = 1024;          // 源图边长
constexpr u32 kTile = 128;              // tile 尺寸
constexpr u32 kBorder = 4;              // border
constexpr u32 kPhysTile = kTile + 2 * kBorder;  // 136
constexpr u32 kAtlasSize = 2048;        // 物理 atlas 边长(15x15 tile 槽)
constexpr u32 kPageTableSize = 16;      // 页表边长(单 mip 打包布局)
constexpr u32 kMipCount = 4;            // 1024/512/256/128

// 页表打包布局:各 mip 层级页格子的列偏移
constexpr u32 kLevelOriginX[kMipCount] = {0, 8, 12, 14};

u32 PackEntry(u32 atlasOriginX, u32 atlasOriginY) {
    return 1u | (atlasOriginX << 8) | (atlasOriginY << 19);
}

// ============================= CPU 参考数据 =============================
// 源图:确定性图案
std::vector<u8> MakeSourceImage() {
    std::vector<u8> img(size_t(kSrcSize) * kSrcSize * 4);
    for (u32 y = 0; y < kSrcSize; ++y) {
        for (u32 x = 0; x < kSrcSize; ++x) {
            u8* p = &img[(size_t(y) * kSrcSize + x) * 4];
            p[0] = u8((x * 7 + y * 13) & 0xFF);
            p[1] = u8((x * 5 ^ y * 3) & 0xFF);
            p[2] = u8((x * 3 + y * 11) & 0xFF);
            p[3] = 255;
        }
    }
    return img;
}

// box-filter mip 链(mips[0] = 源图)
std::vector<std::vector<u8>> MakeMips(const std::vector<u8>& src) {
    std::vector<std::vector<u8>> mips;
    mips.push_back(src);
    u32 side = kSrcSize;   // 显式跟踪边长(mips.back().size()/4 是像素总数,不是边长)
    while (side > kTile) {
        u32 half = side / 2;
        std::vector<u8> next(size_t(half) * half * 4);
        for (u32 y = 0; y < half; ++y) {
            for (u32 x = 0; x < half; ++x) {
                for (u32 c = 0; c < 4; ++c) {
                    u32 sum = mips.back()[((size_t(y * 2) * side) + x * 2) * 4 + c]
                            + mips.back()[((size_t(y * 2) * side) + x * 2 + 1) * 4 + c]
                            + mips.back()[((size_t(y * 2 + 1) * side) + x * 2) * 4 + c]
                            + mips.back()[((size_t(y * 2 + 1) * side) + x * 2 + 1) * 4 + c];
                    next[(size_t(y) * half + x) * 4 + c] = u8((sum + 2) / 4);
                }
            }
        }
        mips.push_back(std::move(next));
        side = half;
    }
    return mips;
}

// 取 (px,py) tile + clamp 扩展 border → kPhysTile² RGBA
std::vector<u8> MakeTileWithBorder(const std::vector<u8>& mip, u32 mipSide,
                                    u32 px, u32 py) {
    std::vector<u8> tile(size_t(kPhysTile) * kPhysTile * 4, 0);
    auto sampleClamped = [&](int sx, int sy) -> const u8* {
        sx = std::clamp(sx, 0, int(mipSide) - 1);
        sy = std::clamp(sy, 0, int(mipSide) - 1);
        return &mip[(size_t(sy) * mipSide + sx) * 4];
    };
    for (u32 y = 0; y < kPhysTile; ++y) {
        for (u32 x = 0; x < kPhysTile; ++x) {
            int sx = int(px * kTile) + int(x) - int(kBorder);
            int sy = int(py * kTile) + int(y) - int(kBorder);
            std::memcpy(&tile[(size_t(y) * kPhysTile + x) * 4], sampleClamped(sx, sy), 4);
        }
    }
    return tile;
}

struct SampleParams { u32 outWidth; u32 vLevel; u32 pad0; u32 pad1; };

} // anonymous namespace

TestResult TestVT_PrototypeEndToEnd() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ---------- 0. CPU 参考 ----------
    std::vector<std::vector<u8>> mips = MakeMips(MakeSourceImage());
    TEST_ASSERT(mips.size() == kMipCount, "4 mips generated");
    const u32 mipSide[kMipCount] = {1024, 512, 256, 128};

    // ---------- 1. 资源创建 ----------
    // 物理 atlas(RGBA8)
    TextureDesc atlasDesc{};
    atlasDesc.size = {kAtlasSize, kAtlasSize, 1};
    atlasDesc.mipLevels = 1;
    atlasDesc.arraySize = 1;
    atlasDesc.format = DataFormat::RGBA8_UNorm;
    atlasDesc.type = TextureType::Texture2D;
    atlasDesc.usage = TextureUsage::ShaderResource;
    atlasDesc.memoryUsage = GPUMemoryUsage::Static;
    atlasDesc.name = "VTProto_Atlas";
    ResourceHandle atlas = fx.base->CreateTexture(atlasDesc);
    TEST_ASSERT(atlas != handles::INVALID_RESOURCE, "CreateTexture atlas");

    // 页表(R32_UINT,采样 + 存储)
    TextureDesc ptDesc{};
    ptDesc.size = {kPageTableSize, kPageTableSize, 1};
    ptDesc.mipLevels = 1;
    ptDesc.arraySize = 1;
    ptDesc.format = DataFormat::R32_UInt;
    ptDesc.type = TextureType::Texture2D;
    ptDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    ptDesc.memoryUsage = GPUMemoryUsage::Static;
    ptDesc.name = "VTProto_PageTable";
    ResourceHandle pageTable = fx.base->CreateTexture(ptDesc);
    TEST_ASSERT(pageTable != handles::INVALID_RESOURCE, "CreateTexture page table");

    // ---------- 2. tile 上传(M0.7 局部上传在 VT 布局下) ----------
    // mip0 8x8 + mip1 4x4 页 → atlas 槽位 (slotX, slotY) → origin = slot * 136
    // 布局:mipL 的 vPage(p) → atlas 槽位 (p.x, p.y + L*8)  (行偏移分带,避免重叠)
    auto atlasOrigin = [](u32 level, u32 px, u32 py) -> std::pair<u32, u32> {
        return {px * kPhysTile, (py + level * 8) * kPhysTile};
    };
    u32 uploadedTiles = 0;
    for (u32 level = 0; level < 2; ++level) {
        u32 pages = mipSide[level] / kTile;
        for (u32 py = 0; py < pages; ++py) {
            for (u32 px = 0; px < pages; ++px) {
                std::vector<u8> tile = MakeTileWithBorder(mips[level], mipSide[level], px, py);
                auto [ox, oy] = atlasOrigin(level, px, py);
                TextureRegionUpdate r{};
                r.offsetX = ox; r.offsetY = oy;
                r.width = kPhysTile; r.height = kPhysTile;
                TEST_ASSERT(fx.base->UpdateTextureRegion(atlas, tile.data(), tile.size(), r),
                            "UpdateTextureRegion tile upload");
                ++uploadedTiles;
            }
        }
    }
    std::cout << "[VTProto] uploaded " << uploadedTiles << " tiles to atlas" << std::endl;

    // 页表 entries(两个批次:mip0 64 项 / mip1 16 项 — 分开 dispatch 验证两轮写→读)
    auto makeEntries = [&](u32 level) {
        u32 pages = mipSide[level] / kTile;
        std::vector<u32> entries;
        entries.push_back(pages * pages);
        for (u32 py = 0; py < pages; ++py)
            for (u32 px = 0; px < pages; ++px) {
                auto [ox, oy] = atlasOrigin(level, px, py);
                entries.push_back(kLevelOriginX[level] + px);  // 页表地址 X(打包布局)
                entries.push_back(py);                          // 页表地址 Y
                entries.push_back(PackEntry(ox, oy));           // 条目值
            }
        return entries;
    };
    std::vector<u32> entries0 = makeEntries(0);
    std::vector<u32> entries1 = makeEntries(1);

    auto makeEntryBuffer = [&](const std::vector<u32>& data, const char* name) {
        BufferDesc d{};
        d.size = data.size() * sizeof(u32);
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = name;
        ResourceHandle h = fx.base->CreateBuffer(d);
        if (h == handles::INVALID_RESOURCE ||
            !fx.base->UpdateBufferData(h, data.data(), d.size, 0)) {
            std::cerr << "[VTProto] entries buffer create/update failed" << std::endl;
            return handles::INVALID_RESOURCE;
        }
        return h;
    };
    ResourceHandle entriesBuf0 = makeEntryBuffer(entries0, "VTProto_Entries0");
    TEST_ASSERT(entriesBuf0 != handles::INVALID_RESOURCE, "valid entriesBuf0");
    ResourceHandle entriesBuf1 = makeEntryBuffer(entries1, "VTProto_Entries1");
    TEST_ASSERT(entriesBuf1 != handles::INVALID_RESOURCE, "valid entriesBuf1");

    // 采样参数(每 pass 独立 UBO — 避免录制中改内容的双 dispatch 竞态)
    auto makeParams = [&](u32 level, const char* name) {
        SampleParams p{mipSide[level], level, 0, 0};
        BufferDesc d{};
        d.size = sizeof(p);
        d.type = BufferType::Constant;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        d.name = name;
        ResourceHandle h = fx.base->CreateBuffer(d);
        if (h == handles::INVALID_RESOURCE ||
            !fx.base->UpdateBufferData(h, &p, sizeof(p), 0)) {
            std::cerr << "[VTProto] params buffer create/update failed" << std::endl;
            return handles::INVALID_RESOURCE;
        }
        return h;
    };
    ResourceHandle params0 = makeParams(0, "VTProto_Params0");
    TEST_ASSERT(params0 != handles::INVALID_RESOURCE, "valid params0");
    ResourceHandle params1 = makeParams(1, "VTProto_Params1");
    TEST_ASSERT(params1 != handles::INVALID_RESOURCE, "valid params1");

    // 输出 buffer(mip0 4MB / mip1 1MB)
    auto makeOut = [&](u32 level, const char* name) {
        BufferDesc d{};
        d.size = u64(mipSide[level]) * mipSide[level] * 4;
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = name;
        ResourceHandle h = fx.base->CreateBuffer(d);
        return h;
    };
    ResourceHandle out0 = makeOut(0, "VTProto_Out0");
    TEST_ASSERT(out0 != handles::INVALID_RESOURCE, "valid out0");
    ResourceHandle out1 = makeOut(1, "VTProto_Out1");
    TEST_ASSERT(out1 != handles::INVALID_RESOURCE, "valid out1");

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.base, u64(kSrcSize) * kSrcSize * 4,
                                rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    // ---------- 3. shader / pipeline / descriptor ----------
    auto updCS = ReadSPV("Assets/Shaders/VTProto_UpdatePageTable.comp.spv");
    auto smpCS = ReadSPV("Assets/Shaders/VTProto_Sample.comp.spv");
    TEST_ASSERT(!updCS.empty() && !smpCS.empty(), "Read VTProto SPIR-V (run build_spv.sh)");
    ShaderHandle updShader = fx.base->CreateShader(updCS.data(), updCS.size(), ShaderStage::Compute, "main");
    ShaderHandle smpShader = fx.base->CreateShader(smpCS.data(), smpCS.size(), ShaderStage::Compute, "main");
    TEST_ASSERT(updShader != handles::INVALID_SHADER && smpShader != handles::INVALID_SHADER, "CreateShader");

    // update 布局: b0 StorageImage(页表) / b1 StorageBuffer(entries)
    DescriptorSetLayoutBinding updBind[2]{};
    updBind[0].binding = 0; updBind[0].descriptorType = DescriptorType::StorageImage;
    updBind[0].descriptorCount = 1; updBind[0].stageFlags = ShaderStage::Compute;
    updBind[1].binding = 1; updBind[1].descriptorType = DescriptorType::StorageBuffer;
    updBind[1].descriptorCount = 1; updBind[1].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc updLayoutDesc{};
    updLayoutDesc.bindingCount = 2;
    updLayoutDesc.bindings = updBind;
    DescriptorSetLayoutHandle updLayout = fx.base->CreateDescriptorSetLayout(updLayoutDesc);
    TEST_ASSERT(updLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "upd layout");

    PipelineLayoutDesc updPLDesc{};
    updPLDesc.setLayoutCount = 1; updPLDesc.setLayouts = &updLayout; updPLDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle updPL = fx.base->CreatePipelineLayout(updPLDesc);
    TEST_ASSERT(updPL != handles::INVALID_PIPELINE_LAYOUT, "upd pipeline layout");

    ComputePipelineDesc updCPD{};
    updCPD.computeShader = updShader; updCPD.layout = updPL;
    updCPD.threadGroupSize = {64, 1, 1};
    PipelineHandle updPipe = fx.base->CreateComputePipeline(updCPD);
    TEST_ASSERT(updPipe != handles::INVALID_PIPELINE, "upd pipeline");

    // sample 布局: b0 SampledImage(页表 uint) / b1 SampledImage(atlas) /
    //               b2 UBO / b3 StorageBuffer(out)
    DescriptorSetLayoutBinding smpBind[4]{};
    smpBind[0].binding = 0; smpBind[0].descriptorType = DescriptorType::SampledImage;
    smpBind[0].descriptorCount = 1; smpBind[0].stageFlags = ShaderStage::Compute;
    smpBind[1].binding = 1; smpBind[1].descriptorType = DescriptorType::SampledImage;
    smpBind[1].descriptorCount = 1; smpBind[1].stageFlags = ShaderStage::Compute;
    smpBind[2].binding = 2; smpBind[2].descriptorType = DescriptorType::UniformBuffer;
    smpBind[2].descriptorCount = 1; smpBind[2].stageFlags = ShaderStage::Compute;
    smpBind[3].binding = 3; smpBind[3].descriptorType = DescriptorType::StorageBuffer;
    smpBind[3].descriptorCount = 1; smpBind[3].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc smpLayoutDesc{};
    smpLayoutDesc.bindingCount = 4;
    smpLayoutDesc.bindings = smpBind;
    DescriptorSetLayoutHandle smpLayout = fx.base->CreateDescriptorSetLayout(smpLayoutDesc);
    TEST_ASSERT(smpLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "smp layout");

    PipelineLayoutDesc smpPLDesc{};
    smpPLDesc.setLayoutCount = 1; smpPLDesc.setLayouts = &smpLayout; smpPLDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle smpPL = fx.base->CreatePipelineLayout(smpPLDesc);
    TEST_ASSERT(smpPL != handles::INVALID_PIPELINE_LAYOUT, "smp pipeline layout");

    ComputePipelineDesc smpCPD{};
    smpCPD.computeShader = smpShader; smpCPD.layout = smpPL;
    smpCPD.threadGroupSize = {8, 8, 1};
    PipelineHandle smpPipe = fx.base->CreateComputePipeline(smpCPD);
    TEST_ASSERT(smpPipe != handles::INVALID_PIPELINE, "smp pipeline");

    // update 描述符集 x2(entries0 / entries1)
    auto makeUpdSet = [&](ResourceHandle entriesBuf) {
        DescriptorSetDesc d{}; d.layout = updLayout;
        DescriptorSetHandle h = fx.base->CreateDescriptorSet(d);
        if (h == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;
        DescriptorImageInfo ptInfo{};
        ptInfo.imageView = pageTable;
        ptInfo.imageLayout = ResourceState::UnorderedAccess;
        DescriptorBufferInfo eInfo{entriesBuf, 0, 0};
        WriteDescriptorSet w[2]{};
        w[0].dstSet = h; w[0].dstBinding = 0; w[0].dstArrayElement = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = DescriptorType::StorageImage;
        w[0].imageInfo = &ptInfo;
        w[1].dstSet = h; w[1].dstBinding = 1; w[1].dstArrayElement = 0;
        w[1].descriptorCount = 1; w[1].descriptorType = DescriptorType::StorageBuffer;
        w[1].bufferInfo = &eInfo;
        fx.base->UpdateDescriptorSets(2, w);
        return h;
    };
    DescriptorSetHandle updSet0 = makeUpdSet(entriesBuf0);
    TEST_ASSERT(updSet0 != handles::INVALID_DESCRIPTOR_SET, "valid updSet0");
    DescriptorSetHandle updSet1 = makeUpdSet(entriesBuf1);
    TEST_ASSERT(updSet1 != handles::INVALID_DESCRIPTOR_SET, "valid updSet1");

    // sample 描述符集 x2(params/out 各异)
    auto makeSmpSet = [&](ResourceHandle paramsBuf, ResourceHandle outBuf) {
        DescriptorSetDesc d{}; d.layout = smpLayout;
        DescriptorSetHandle h = fx.base->CreateDescriptorSet(d);
        if (h == handles::INVALID_DESCRIPTOR_SET) return handles::INVALID_DESCRIPTOR_SET;
        DescriptorImageInfo ptInfo{};
        ptInfo.imageView = pageTable;
        ptInfo.imageLayout = ResourceState::ShaderResource;
        DescriptorImageInfo atInfo{};
        atInfo.imageView = atlas;
        atInfo.imageLayout = ResourceState::ShaderResource;
        DescriptorBufferInfo pInfo{paramsBuf, 0, sizeof(SampleParams)};
        DescriptorBufferInfo oInfo{outBuf, 0, 0};
        WriteDescriptorSet w[4]{};
        w[0].dstSet = h; w[0].dstBinding = 0; w[0].dstArrayElement = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = DescriptorType::SampledImage;
        w[0].imageInfo = &ptInfo;
        w[1].dstSet = h; w[1].dstBinding = 1; w[1].dstArrayElement = 0;
        w[1].descriptorCount = 1; w[1].descriptorType = DescriptorType::SampledImage;
        w[1].imageInfo = &atInfo;
        w[2].dstSet = h; w[2].dstBinding = 2; w[2].dstArrayElement = 0;
        w[2].descriptorCount = 1; w[2].descriptorType = DescriptorType::UniformBuffer;
        w[2].bufferInfo = &pInfo;
        w[3].dstSet = h; w[3].dstBinding = 3; w[3].dstArrayElement = 0;
        w[3].descriptorCount = 1; w[3].descriptorType = DescriptorType::StorageBuffer;
        w[3].bufferInfo = &oInfo;
        fx.base->UpdateDescriptorSets(4, w);
        return h;
    };
    DescriptorSetHandle smpSet0 = makeSmpSet(params0, out0);
    TEST_ASSERT(smpSet0 != handles::INVALID_DESCRIPTOR_SET, "valid smpSet0");
    DescriptorSetHandle smpSet1 = makeSmpSet(params1, out1);
    TEST_ASSERT(smpSet1 != handles::INVALID_DESCRIPTOR_SET, "valid smpSet1");

    auto imageBarrier = [&](VulkanCommandBuffer* vcmd, ResourceHandle tex,
                            ResourceState before, ResourceState after) {
        ResourceBarrier b{};
        b.resource = tex;
        b.beforeState = before;
        b.afterState = after;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&b, 1);
    };

    // ---------- 4. 帧 1:页表更新(两轮)+ 两级采样,同一 cmdbuf ----------
    {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin frame1");

        // 页表 Unknown → UnorderedAccess,更新 mip0 页
        imageBarrier(vcmd, pageTable, ResourceState::Unknown, ResourceState::UnorderedAccess);
        vcmd->BindComputePipeline(updPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet0, 0, nullptr);
        vcmd->Dispatch((entries0[0] + 63) / 64, 1, 1);

        // 写 → 读(M0.12 同帧一致性)
        imageBarrier(vcmd, pageTable, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                            AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

        vcmd->BindComputePipeline(smpPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet0, 0, nullptr);
        vcmd->Dispatch(mipSide[0] / 8, mipSide[0] / 8, 1);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                            AccessFlag::ShaderWrite, AccessFlag::TransferRead);
        vcmd->CopyBuffer(out0, ring.buffer(0), 0, 0, u64(mipSide[0]) * mipSide[0] * 4);

        // 第二轮:更新 mip1 页 → 采样 mip1
        imageBarrier(vcmd, pageTable, ResourceState::ShaderResource, ResourceState::UnorderedAccess);
        vcmd->BindComputePipeline(updPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet1, 0, nullptr);
        vcmd->Dispatch((entries1[0] + 63) / 64, 1, 1);
        imageBarrier(vcmd, pageTable, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                            AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

        vcmd->BindComputePipeline(smpPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet1, 0, nullptr);
        vcmd->Dispatch(mipSide[1] / 8, mipSide[1] / 8, 1);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                            AccessFlag::ShaderWrite, AccessFlag::TransferRead);
        vcmd->CopyBuffer(out1, ring.buffer(1), 0, 0, u64(mipSide[1]) * mipSide[1] * 4);

        TEST_ASSERT(vcmd->End(), "End frame1");
        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, 0);
        TEST_ASSERT(fx.base->Submit(si), "Submit frame1");
        TEST_ASSERT(ring.wait(fx.base, 0, 5000), "ring.wait slot0");
        fx.base->DestroyCommandBuffer(cmd);   // free 必须在 fence 等待之后(GPU 执行完)
    }

    // ---------- 5. 校验 mip0 / mip1 ----------
    auto verifyLevel = [&](u32 level, u32 slot) {
        const u32* mapped = static_cast<const u32*>(ring.map(fx.base, slot));
        TEST_ASSERT(mapped != nullptr, "ring.map");
        u32 side = mipSide[level];
        u64 errs = 0; int maxDelta = 0;
        for (u32 y = 0; y < side; ++y) {
            for (u32 x = 0; x < side; ++x) {
                u32 got = mapped[size_t(y) * side + x];
                const u8* expect = &mips[level][(size_t(y) * side + x) * 4];
                u32 e = u32(expect[0]) | (u32(expect[1]) << 8) | (u32(expect[2]) << 16) | (u32(expect[3]) << 24);
                if (got != e) {
                    // RGBA8_UNorm 规格化往返允许 ±1
                    int d = std::max({std::abs(int(got & 0xFF) - int(e & 0xFF)),
                                      std::abs(int((got >> 8) & 0xFF) - int((e >> 8) & 0xFF)),
                                      std::abs(int((got >> 16) & 0xFF) - int((e >> 16) & 0xFF)),
                                      std::abs(int((got >> 24) & 0xFF) - int((e >> 24) & 0xFF))});
                    maxDelta = std::max(maxDelta, d);
                    if (d > 1) {
                        ++errs;
                        if (errs <= 4)
                            std::cerr << "[VTProto] L" << level << " (" << x << "," << y << ") got 0x"
                                      << std::hex << got << " expect 0x" << e << std::dec << std::endl;
                    }
                }
            }
        }
        ring.unmap(fx.base, slot);
        std::cout << "[VTProto] mip" << level << ": " << errs << " mismatches (maxDelta="
                  << maxDelta << ")" << std::endl;
        TEST_ASSERT(errs == 0, "mip matches reference");
    };
    verifyLevel(0, 0);
    verifyLevel(1, 1);

    // ---------- 6. 帧 2:补丁 tile + 跨 cmdbuf 页表更新可见性 ----------
    {
        // 修改 mip0 vPage(3,3) 的参考内容(整体 +0x40 偏移),上传到新 atlas 槽位 (14,14)
        const u32 patchPX = 3, patchPY = 3;
        for (u32 y = patchPY * kTile; y < (patchPY + 1) * kTile; ++y)
            for (u32 x = patchPX * kTile; x < (patchPX + 1) * kTile; ++x) {
                u8* p = &mips[0][(size_t(y) * kSrcSize + x) * 4];
                p[0] = u8(p[0] + 0x40); p[1] = u8(p[1] + 0x20); p[2] = u8(p[2] ^ 0x55);
            }
        std::vector<u8> patchTile = MakeTileWithBorder(mips[0], mipSide[0], patchPX, patchPY);
        const u32 newOX = 14 * kPhysTile, newOY = (14 + 0 * 8) * kPhysTile;
        TextureRegionUpdate r{};
        r.offsetX = newOX; r.offsetY = newOY; r.width = kPhysTile; r.height = kPhysTile;
        TEST_ASSERT(fx.base->UpdateTextureRegion(atlas, patchTile.data(), patchTile.size(), r),
                    "patch tile upload");

        // 页表项重定向到新槽位(独立 entries buffer)
        std::vector<u32> entries2;
        entries2.push_back(1);
        entries2.push_back(kLevelOriginX[0] + patchPX);
        entries2.push_back(patchPY);
        entries2.push_back(PackEntry(newOX, newOY));
        ResourceHandle entriesBuf2 = makeEntryBuffer(entries2, "VTProto_Entries2");
    TEST_ASSERT(entriesBuf2 != handles::INVALID_RESOURCE, "valid entriesBuf2");
        DescriptorSetHandle updSet2 = makeUpdSet(entriesBuf2);
    TEST_ASSERT(updSet2 != handles::INVALID_DESCRIPTOR_SET, "valid updSet2");

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin frame2");

        imageBarrier(vcmd, pageTable, ResourceState::ShaderResource, ResourceState::UnorderedAccess);
        vcmd->BindComputePipeline(updPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet2, 0, nullptr);
        vcmd->Dispatch(1, 1, 1);
        imageBarrier(vcmd, pageTable, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                            AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

        vcmd->BindComputePipeline(smpPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet0, 0, nullptr);
        vcmd->Dispatch(mipSide[0] / 8, mipSide[0] / 8, 1);
        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                            AccessFlag::ShaderWrite, AccessFlag::TransferRead);
        vcmd->CopyBuffer(out0, ring.buffer(2), 0, 0, u64(mipSide[0]) * mipSide[0] * 4);

        TEST_ASSERT(vcmd->End(), "End frame2");
        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, 2);
        TEST_ASSERT(fx.base->Submit(si), "Submit frame2");
        TEST_ASSERT(ring.wait(fx.base, 2, 5000), "ring.wait slot2");
        fx.base->DestroyCommandBuffer(cmd);
        verifyLevel(0, 2);   // 补丁 tile + 其余 63 tile 全对 → 跨帧页表更新正确

        fx.base->DestroyDescriptorSet(updSet2);
        fx.base->DestroyBuffer(entriesBuf2);
    }

    // ---------- 清理 ----------
    ring.shutdown(fx.base);
    fx.base->DestroyDescriptorSet(smpSet0);
    fx.base->DestroyDescriptorSet(smpSet1);
    fx.base->DestroyDescriptorSet(updSet0);
    fx.base->DestroyDescriptorSet(updSet1);
    fx.base->DestroyDescriptorSetLayout(smpLayout);
    fx.base->DestroyDescriptorSetLayout(updLayout);
    fx.base->DestroyPipelineLayout(smpPL);
    fx.base->DestroyPipelineLayout(updPL);
    fx.base->DestroyPipeline(smpPipe);
    fx.base->DestroyPipeline(updPipe);
    fx.base->DestroyBuffer(out0);
    fx.base->DestroyBuffer(out1);
    fx.base->DestroyBuffer(params0);
    fx.base->DestroyBuffer(params1);
    fx.base->DestroyBuffer(entriesBuf0);
    fx.base->DestroyBuffer(entriesBuf1);
    fx.base->DestroyTexture(pageTable);
    fx.base->DestroyTexture(atlas);
    fx.base->DestroyShader(updShader);
    fx.base->DestroyShader(smpShader);
    return TestResult::Passed;
}

void RegisterVulkanVTPrototypeTests() {
    auto suite = std::make_shared<TestSuite>("VulkanVTPrototypeTests");
    suite->AddTestCase(TestCase("EndToEnd", TestVT_PrototypeEndToEnd));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanVTPrototypeTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanVTPrototype] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
