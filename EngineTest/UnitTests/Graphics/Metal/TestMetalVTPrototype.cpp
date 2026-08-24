/**
 * @file TestMetalVTPrototype.cpp
 * @brief VT M0.9-0.12 — 端到端原型(Metal),与 TestVulkanVTPrototype.cpp 对等。
 * @details 同样的 CPU 驱动流送闭环:页表 CS 更新(imageStore 等价的
 *          texture2d<uint>::write)+ 两段式采样 + 逐像素精确比对 +
 *          跨命令缓冲页表更新可见性。差异点:Metal 无显式图像布局屏障
 *          (UAV 纹理强制 HazardTrackingTracked,计算编码器间自动依赖)。
 *
 * 绑定约定(与 Vulkan 版 shader 一致):
 *   update:  texture(0)=页表(read_write)  buffer(1)=entries
 *   sample:  texture(0)=页表              texture(1)=atlas
 *            buffer(2)=params             buffer(3)=out
 */

#ifdef __APPLE__

#include "../../TestFramework.h"

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIReadbackRing.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

namespace {

constexpr u32 kSrcSize = 1024;
constexpr u32 kTile = 128;
constexpr u32 kBorder = 4;
constexpr u32 kPhysTile = kTile + 2 * kBorder;
constexpr u32 kAtlasSize = 2048;
constexpr u32 kPageTableSize = 16;
constexpr u32 kMipCount = 4;
constexpr u32 kLevelOriginX[kMipCount] = {0, 8, 12, 14};

u32 PackEntry(u32 ox, u32 oy) { return 1u | (ox << 8) | (oy << 19); }

std::vector<u8> MakeSourceImage() {
    std::vector<u8> img(size_t(kSrcSize) * kSrcSize * 4);
    for (u32 y = 0; y < kSrcSize; ++y)
        for (u32 x = 0; x < kSrcSize; ++x) {
            u8* p = &img[(size_t(y) * kSrcSize + x) * 4];
            p[0] = u8((x * 7 + y * 13) & 0xFF);
            p[1] = u8((x * 5 ^ y * 3) & 0xFF);
            p[2] = u8((x * 3 + y * 11) & 0xFF);
            p[3] = 255;
        }
    return img;
}

std::vector<std::vector<u8>> MakeMips(const std::vector<u8>& src) {
    std::vector<std::vector<u8>> mips;
    mips.push_back(src);
    u32 side = kSrcSize;   // 显式跟踪边长(mips.back().size()/4 是像素总数,不是边长)
    while (side > kTile) {
        u32 half = side / 2;
        std::vector<u8> next(size_t(half) * half * 4);
        for (u32 y = 0; y < half; ++y)
            for (u32 x = 0; x < half; ++x)
                for (u32 c = 0; c < 4; ++c) {
                    u32 sum = mips.back()[((size_t(y * 2) * side) + x * 2) * 4 + c]
                            + mips.back()[((size_t(y * 2) * side) + x * 2 + 1) * 4 + c]
                            + mips.back()[((size_t(y * 2 + 1) * side) + x * 2) * 4 + c]
                            + mips.back()[((size_t(y * 2 + 1) * side) + x * 2 + 1) * 4 + c];
                    next[(size_t(y) * half + x) * 4 + c] = u8((sum + 2) / 4);
                }
        mips.push_back(std::move(next));
        side = half;
    }
    return mips;
}

std::vector<u8> MakeTileWithBorder(const std::vector<u8>& mip, u32 mipSide, u32 px, u32 py) {
    std::vector<u8> tile(size_t(kPhysTile) * kPhysTile * 4, 0);
    auto sampleClamped = [&](int sx, int sy) -> const u8* {
        sx = std::clamp(sx, 0, int(mipSide) - 1);
        sy = std::clamp(sy, 0, int(mipSide) - 1);
        return &mip[(size_t(sy) * mipSide + sx) * 4];
    };
    for (u32 y = 0; y < kPhysTile; ++y)
        for (u32 x = 0; x < kPhysTile; ++x) {
            int sx = int(px * kTile) + int(x) - int(kBorder);
            int sy = int(py * kTile) + int(y) - int(kBorder);
            std::memcpy(&tile[(size_t(y) * kPhysTile + x) * 4], sampleClamped(sx, sy), 4);
        }
    return tile;
}

struct SampleParams { u32 outWidth; u32 vLevel; u32 pad0; u32 pad1; };

// ============================================================================
// 嵌入式 MSL(与 GLSL VTProto_*.comp 语义一致)
// ============================================================================
const char* kUpdateCS = R"(
#include <metal_stdlib>
using namespace metal;

kernel void vtProtoUpdateCS(texture2d<uint, access::read_write> pageTable [[texture(0)]],
                            device const uint* entries [[buffer(1)]],
                            uint gid [[thread_position_in_grid]])
{
    uint count = entries[0];
    if (gid >= count) return;
    uint ptX = entries[1 + gid * 3 + 0];
    uint ptY = entries[1 + gid * 3 + 1];
    uint val = entries[1 + gid * 3 + 2];
    pageTable.write(uint4(val, 0u, 0u, 0u), ushort2(ptX, ptY), 0);
}
)";

const char* kSampleCS = R"(
#include <metal_stdlib>
using namespace metal;

kernel void vtProtoSampleCS(texture2d<uint, access::sample> pageTable [[texture(0)]],
                            texture2d<float, access::sample> atlas [[texture(1)]],
                            constant uint4& params [[buffer(2)]],
                            device uint* outBuf [[buffer(3)]],
                            uint2 xy [[thread_position_in_grid]])
{
    if (xy.x >= params.x || xy.y >= params.x) return;
    uint level = params.y;
    uint ox = (level == 0u) ? 0u : ((level == 1u) ? 8u : ((level == 2u) ? 12u : 14u));
    uint2 vPage = xy / 128u;
    uint2 fracPx = xy % 128u;
    uint entry = pageTable.read(ushort2(vPage.x + ox, vPage.y), 0).r;
    uint idx = xy.y * params.x + xy.x;
    if ((entry & 1u) == 0u) { outBuf[idx] = 0u; return; }
    uint originX = (entry >> 8) & 0x7FFu;
    uint originY = (entry >> 19) & 0x7FFu;
    float4 c = atlas.read(ushort2(originX + 4u + fracPx.x, originY + 4u + fracPx.y), 0);
    uint4 b = uint4(clamp(c * 255.0 + 0.5, 0.0, 255.0));
    outBuf[idx] = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
)";

struct MetalFixture {
    DeviceDesc desc{};
    MetalDevice* device{nullptr};
    bool Init() {
        desc.platform = RHIPlatform::Metal;
        desc.enableDebug = true;
        device = new MetalDevice(desc);
        if (!device->Initialize() || !device->IsValid()) {
            delete device; device = nullptr;
            return false;
        }
        return true;
    }
    ~MetalFixture() { if (device) { device->Shutdown(); delete device; } }
};

} // anonymous namespace

TestResult TestVT_PrototypeEndToEnd_Metal() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "Metal device init");

    std::vector<std::vector<u8>> mips = MakeMips(MakeSourceImage());
    TEST_ASSERT(mips.size() == kMipCount, "4 mips generated");
    const u32 mipSide[kMipCount] = {1024, 512, 256, 128};

    // ---------- 资源 ----------
    TextureDesc atlasDesc{};
    atlasDesc.size = {kAtlasSize, kAtlasSize, 1};
    atlasDesc.mipLevels = 1;
    atlasDesc.arraySize = 1;
    atlasDesc.format = DataFormat::RGBA8_UNorm;
    atlasDesc.type = TextureType::Texture2D;
    atlasDesc.usage = TextureUsage::ShaderResource;
    atlasDesc.memoryUsage = GPUMemoryUsage::Static;
    atlasDesc.name = "VTProto_Atlas";
    ResourceHandle atlas = fx.device->CreateTexture(atlasDesc);
    TEST_ASSERT(atlas != handles::INVALID_RESOURCE, "CreateTexture atlas");

    TextureDesc ptDesc{};
    ptDesc.size = {kPageTableSize, kPageTableSize, 1};
    ptDesc.mipLevels = 1;
    ptDesc.arraySize = 1;
    ptDesc.format = DataFormat::R32_UInt;
    ptDesc.type = TextureType::Texture2D;
    ptDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    ptDesc.memoryUsage = GPUMemoryUsage::Static;
    ptDesc.name = "VTProto_PageTable";
    ResourceHandle pageTable = fx.device->CreateTexture(ptDesc);
    TEST_ASSERT(pageTable != handles::INVALID_RESOURCE, "CreateTexture page table");

    auto atlasOrigin = [](u32 level, u32 px, u32 py) -> std::pair<u32, u32> {
        return {px * kPhysTile, (py + level * 8) * kPhysTile};
    };
    u32 uploadedTiles = 0;
    for (u32 level = 0; level < 2; ++level) {
        u32 pages = mipSide[level] / kTile;
        for (u32 py = 0; py < pages; ++py)
            for (u32 px = 0; px < pages; ++px) {
                std::vector<u8> tile = MakeTileWithBorder(mips[level], mipSide[level], px, py);
                auto [ox, oy] = atlasOrigin(level, px, py);
                TextureRegionUpdate r{};
                r.offsetX = ox; r.offsetY = oy;
                r.width = kPhysTile; r.height = kPhysTile;
                TEST_ASSERT(fx.device->UpdateTextureRegion(atlas, tile.data(), tile.size(), r),
                            "UpdateTextureRegion tile upload");
                ++uploadedTiles;
            }
    }
    std::cout << "[VTProto/Metal] uploaded " << uploadedTiles << " tiles to atlas" << std::endl;

    auto makeEntries = [&](u32 level) {
        u32 pages = mipSide[level] / kTile;
        std::vector<u32> entries;
        entries.push_back(pages * pages);
        for (u32 py = 0; py < pages; ++py)
            for (u32 px = 0; px < pages; ++px) {
                auto [ox, oy] = atlasOrigin(level, px, py);
                entries.push_back(kLevelOriginX[level] + px);
                entries.push_back(py);
                entries.push_back(PackEntry(ox, oy));
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
        ResourceHandle h = fx.device->CreateBuffer(d);
        if (h == handles::INVALID_RESOURCE ||
            !fx.device->UpdateBufferData(h, data.data(), d.size, 0)) {
            std::cerr << "[VTProto] entries buffer create/update failed" << std::endl;
            return handles::INVALID_RESOURCE;
        }
        return h;
    };
    ResourceHandle entriesBuf0 = makeEntryBuffer(entries0, "VTProto_Entries0");
    TEST_ASSERT(entriesBuf0 != handles::INVALID_RESOURCE, "valid entriesBuf0");
    ResourceHandle entriesBuf1 = makeEntryBuffer(entries1, "VTProto_Entries1");
    TEST_ASSERT(entriesBuf1 != handles::INVALID_RESOURCE, "valid entriesBuf1");

    auto makeParams = [&](u32 level, const char* name) {
        SampleParams p{mipSide[level], level, 0, 0};
        BufferDesc d{};
        d.size = sizeof(p);
        d.type = BufferType::Constant;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        d.name = name;
        ResourceHandle h = fx.device->CreateBuffer(d);
        if (h == handles::INVALID_RESOURCE ||
            !fx.device->UpdateBufferData(h, &p, sizeof(p), 0)) {
            std::cerr << "[VTProto] params buffer create/update failed" << std::endl;
            return handles::INVALID_RESOURCE;
        }
        return h;
    };
    ResourceHandle params0 = makeParams(0, "VTProto_Params0");
    TEST_ASSERT(params0 != handles::INVALID_RESOURCE, "valid params0");
    ResourceHandle params1 = makeParams(1, "VTProto_Params1");
    TEST_ASSERT(params1 != handles::INVALID_RESOURCE, "valid params1");

    auto makeOut = [&](u32 level, const char* name) {
        BufferDesc d{};
        d.size = u64(mipSide[level]) * mipSide[level] * 4;
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = name;
        ResourceHandle h = fx.device->CreateBuffer(d);
        return h;
    };
    ResourceHandle out0 = makeOut(0, "VTProto_Out0");
    TEST_ASSERT(out0 != handles::INVALID_RESOURCE, "valid out0");
    ResourceHandle out1 = makeOut(1, "VTProto_Out1");
    TEST_ASSERT(out1 != handles::INVALID_RESOURCE, "valid out1");

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.device, u64(kSrcSize) * kSrcSize * 4,
                                rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    // ---------- shader / pipeline / descriptor ----------
    ShaderHandle updShader = fx.device->CreateShader(kUpdateCS, strlen(kUpdateCS),
                                                     ShaderStage::Compute, "vtProtoUpdateCS");
    ShaderHandle smpShader = fx.device->CreateShader(kSampleCS, strlen(kSampleCS),
                                                     ShaderStage::Compute, "vtProtoSampleCS");
    TEST_ASSERT(updShader != handles::INVALID_SHADER && smpShader != handles::INVALID_SHADER, "CreateShader");

    DescriptorSetLayoutBinding updBind[2]{};
    updBind[0].binding = 0; updBind[0].descriptorType = DescriptorType::StorageImage;
    updBind[0].descriptorCount = 1; updBind[0].stageFlags = ShaderStage::Compute;
    updBind[1].binding = 1; updBind[1].descriptorType = DescriptorType::StorageBuffer;
    updBind[1].descriptorCount = 1; updBind[1].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc updLayoutDesc{};
    updLayoutDesc.bindingCount = 2;
    updLayoutDesc.bindings = updBind;
    DescriptorSetLayoutHandle updLayout = fx.device->CreateDescriptorSetLayout(updLayoutDesc);
    TEST_ASSERT(updLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "upd layout");

    PipelineLayoutDesc updPLDesc{};
    updPLDesc.setLayoutCount = 1; updPLDesc.setLayouts = &updLayout; updPLDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle updPL = fx.device->CreatePipelineLayout(updPLDesc);
    TEST_ASSERT(updPL != handles::INVALID_PIPELINE_LAYOUT, "upd pipeline layout");

    ComputePipelineDesc updCPD{};
    updCPD.computeShader = updShader; updCPD.layout = updPL;
    updCPD.threadGroupSize = {64, 1, 1};
    PipelineHandle updPipe = fx.device->CreateComputePipeline(updCPD);
    TEST_ASSERT(updPipe != handles::INVALID_PIPELINE, "upd pipeline");

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
    DescriptorSetLayoutHandle smpLayout = fx.device->CreateDescriptorSetLayout(smpLayoutDesc);
    TEST_ASSERT(smpLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "smp layout");

    PipelineLayoutDesc smpPLDesc{};
    smpPLDesc.setLayoutCount = 1; smpPLDesc.setLayouts = &smpLayout; smpPLDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle smpPL = fx.device->CreatePipelineLayout(smpPLDesc);
    TEST_ASSERT(smpPL != handles::INVALID_PIPELINE_LAYOUT, "smp pipeline layout");

    ComputePipelineDesc smpCPD{};
    smpCPD.computeShader = smpShader; smpCPD.layout = smpPL;
    smpCPD.threadGroupSize = {8, 8, 1};
    PipelineHandle smpPipe = fx.device->CreateComputePipeline(smpCPD);
    TEST_ASSERT(smpPipe != handles::INVALID_PIPELINE, "smp pipeline");

    auto makeUpdSet = [&](ResourceHandle entriesBuf) {
        DescriptorSetDesc d{}; d.layout = updLayout;
        DescriptorSetHandle h = fx.device->CreateDescriptorSet(d);
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
        fx.device->UpdateDescriptorSets(2, w);
        return h;
    };
    DescriptorSetHandle updSet0 = makeUpdSet(entriesBuf0);
    TEST_ASSERT(updSet0 != handles::INVALID_DESCRIPTOR_SET, "valid updSet0");
    DescriptorSetHandle updSet1 = makeUpdSet(entriesBuf1);
    TEST_ASSERT(updSet1 != handles::INVALID_DESCRIPTOR_SET, "valid updSet1");

    auto makeSmpSet = [&](ResourceHandle paramsBuf, ResourceHandle outBuf) {
        DescriptorSetDesc d{}; d.layout = smpLayout;
        DescriptorSetHandle h = fx.device->CreateDescriptorSet(d);
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
        fx.device->UpdateDescriptorSets(4, w);
        return h;
    };
    DescriptorSetHandle smpSet0 = makeSmpSet(params0, out0);
    TEST_ASSERT(smpSet0 != handles::INVALID_DESCRIPTOR_SET, "valid smpSet0");
    DescriptorSetHandle smpSet1 = makeSmpSet(params1, out1);
    TEST_ASSERT(smpSet1 != handles::INVALID_DESCRIPTOR_SET, "valid smpSet1");

    // ---------- 帧 1:两轮页表更新 + 两级采样(Metal 自动 hazard tracking) ----------
    {
        CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin frame1");

        mcmd->BindComputePipeline(updPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet0, 0, nullptr);
        mcmd->Dispatch((entries0[0] + 63) / 64, 1, 1);

        mcmd->BindComputePipeline(smpPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet0, 0, nullptr);
        mcmd->Dispatch(mipSide[0] / 8, mipSide[0] / 8, 1);
        mcmd->CopyBuffer(out0, ring.buffer(0), 0, 0, u64(mipSide[0]) * mipSide[0] * 4);

        mcmd->BindComputePipeline(updPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet1, 0, nullptr);
        mcmd->Dispatch((entries1[0] + 63) / 64, 1, 1);

        mcmd->BindComputePipeline(smpPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet1, 0, nullptr);
        mcmd->Dispatch(mipSide[1] / 8, mipSide[1] / 8, 1);
        mcmd->CopyBuffer(out1, ring.buffer(1), 0, 0, u64(mipSide[1]) * mipSide[1] * 4);

        TEST_ASSERT(mcmd->End(), "End frame1");
        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, 0);
        TEST_ASSERT(fx.device->Submit(si), "Submit frame1");
        TEST_ASSERT(ring.wait(fx.device, 0, 5000), "ring.wait slot0");
        fx.device->DestroyCommandBuffer(cmd);   // free 必须在 fence 等待之后(GPU 执行完)
    }

    auto verifyLevel = [&](u32 level, u32 slot) {
        const u32* mapped = static_cast<const u32*>(ring.map(fx.device, slot));
        TEST_ASSERT(mapped != nullptr, "ring.map");
        u32 side = mipSide[level];
        u64 errs = 0; int maxDelta = 0;
        for (u32 y = 0; y < side; ++y)
            for (u32 x = 0; x < side; ++x) {
                u32 got = mapped[size_t(y) * side + x];
                const u8* expect = &mips[level][(size_t(y) * side + x) * 4];
                u32 e = u32(expect[0]) | (u32(expect[1]) << 8) | (u32(expect[2]) << 16) | (u32(expect[3]) << 24);
                if (got != e) {
                    int d = std::max({std::abs(int(got & 0xFF) - int(e & 0xFF)),
                                      std::abs(int((got >> 8) & 0xFF) - int((e >> 8) & 0xFF)),
                                      std::abs(int((got >> 16) & 0xFF) - int((e >> 16) & 0xFF)),
                                      std::abs(int((got >> 24) & 0xFF) - int((e >> 24) & 0xFF))});
                    maxDelta = std::max(maxDelta, d);
                    if (d > 1) {
                        ++errs;
                        if (errs <= 4)
                            std::cerr << "[VTProto/Metal] L" << level << " (" << x << "," << y
                                      << ") got 0x" << std::hex << got << " expect 0x" << e
                                      << std::dec << std::endl;
                    }
                }
            }
        ring.unmap(fx.device, slot);
        std::cout << "[VTProto/Metal] mip" << level << ": " << errs << " mismatches (maxDelta="
                  << maxDelta << ")" << std::endl;
        TEST_ASSERT(errs == 0, "mip matches reference");
    };
    verifyLevel(0, 0);
    verifyLevel(1, 1);

    // ---------- 帧 2:补丁 tile + 跨 cmdbuf 页表更新可见性 ----------
    {
        const u32 patchPX = 3, patchPY = 3;
        for (u32 y = patchPY * kTile; y < (patchPY + 1) * kTile; ++y)
            for (u32 x = patchPX * kTile; x < (patchPX + 1) * kTile; ++x) {
                u8* p = &mips[0][(size_t(y) * kSrcSize + x) * 4];
                p[0] = u8(p[0] + 0x40); p[1] = u8(p[1] + 0x20); p[2] = u8(p[2] ^ 0x55);
            }
        std::vector<u8> patchTile = MakeTileWithBorder(mips[0], mipSide[0], patchPX, patchPY);
        const u32 newOX = 14 * kPhysTile, newOY = 14 * kPhysTile;
        TextureRegionUpdate r{};
        r.offsetX = newOX; r.offsetY = newOY; r.width = kPhysTile; r.height = kPhysTile;
        TEST_ASSERT(fx.device->UpdateTextureRegion(atlas, patchTile.data(), patchTile.size(), r),
                    "patch tile upload");

        std::vector<u32> entries2;
        entries2.push_back(1);
        entries2.push_back(kLevelOriginX[0] + patchPX);
        entries2.push_back(patchPY);
        entries2.push_back(PackEntry(newOX, newOY));
        ResourceHandle entriesBuf2 = makeEntryBuffer(entries2, "VTProto_Entries2");
    TEST_ASSERT(entriesBuf2 != handles::INVALID_RESOURCE, "valid entriesBuf2");
        DescriptorSetHandle updSet2 = makeUpdSet(entriesBuf2);
    TEST_ASSERT(updSet2 != handles::INVALID_DESCRIPTOR_SET, "valid updSet2");

        CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin frame2");

        mcmd->BindComputePipeline(updPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, updPL, 0, 1, &updSet2, 0, nullptr);
        mcmd->Dispatch(1, 1, 1);

        mcmd->BindComputePipeline(smpPipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, smpPL, 0, 1, &smpSet0, 0, nullptr);
        mcmd->Dispatch(mipSide[0] / 8, mipSide[0] / 8, 1);
        mcmd->CopyBuffer(out0, ring.buffer(2), 0, 0, u64(mipSide[0]) * mipSide[0] * 4);

        TEST_ASSERT(mcmd->End(), "End frame2");
        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, 2);
        TEST_ASSERT(fx.device->Submit(si), "Submit frame2");
        TEST_ASSERT(ring.wait(fx.device, 2, 5000), "ring.wait slot2");
        fx.device->DestroyCommandBuffer(cmd);
        verifyLevel(0, 2);

        fx.device->DestroyDescriptorSet(updSet2);
        fx.device->DestroyBuffer(entriesBuf2);
    }

    ring.shutdown(fx.device);
    fx.device->DestroyDescriptorSet(smpSet0);
    fx.device->DestroyDescriptorSet(smpSet1);
    fx.device->DestroyDescriptorSet(updSet0);
    fx.device->DestroyDescriptorSet(updSet1);
    fx.device->DestroyDescriptorSetLayout(smpLayout);
    fx.device->DestroyDescriptorSetLayout(updLayout);
    fx.device->DestroyPipelineLayout(smpPL);
    fx.device->DestroyPipelineLayout(updPL);
    fx.device->DestroyPipeline(smpPipe);
    fx.device->DestroyPipeline(updPipe);
    fx.device->DestroyBuffer(out0);
    fx.device->DestroyBuffer(out1);
    fx.device->DestroyBuffer(params0);
    fx.device->DestroyBuffer(params1);
    fx.device->DestroyBuffer(entriesBuf0);
    fx.device->DestroyBuffer(entriesBuf1);
    fx.device->DestroyTexture(pageTable);
    fx.device->DestroyTexture(atlas);
    fx.device->DestroyShader(updShader);
    fx.device->DestroyShader(smpShader);
    return TestResult::Passed;
}

void RegisterMetalVTPrototypeTests() {
    auto suite = std::make_shared<TestSuite>("MetalVTPrototypeTests");
    suite->AddTestCase(TestCase("EndToEnd", TestVT_PrototypeEndToEnd_Metal));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterMetalVTPrototypeTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

int main() {
    std::cout << "[TestMetalVTPrototype] not Apple platform — no-op." << std::endl;
    return 0;
}

#endif // __APPLE__
