/**
 * @file TestVTSVTRuntime.cpp
 * @brief VT M2 — SVT 运行时闭环测试(Vulkan/MoltenVK)
 * @details 用 VirtualTextureSystem 跑完整的 feedback 驱动流送循环:
 *
 *   Phase 1 正确性:256² 纹理,全图视角采样 8 帧 →
 *     - feedback → 去重分析 → 流送(解压)→ atlas 上传 → 页表 CS 更新
 *     - 采样输出 PSNR ≥ 28dB(ASTC 4x4 有损压缩容忍)
 *     - 驻留 5 页(4 mip0 + root),零淘汰
 *   Phase 2 淘汰:512² 纹理(21 页) + 512² 小页池(3x3=9 槽) →
 *     - 四象限相机轮转,池占用恒 ≤ 容量,evictions > 0,root 常驻
 *   收尾:release/shutdown 无泄漏(计数归零)。
 */

#include "../../TestFramework.h"
#include "VTBaker.h"
#include "Graphics/VirtualTexture/VirtualTextureSystem.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#endif

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

using namespace primal;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi;
using namespace primal::graphics::vt;
using namespace primal::content;
using namespace Engine::Test;

namespace {

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

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        return base != nullptr;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

std::vector<u8> make_source(u32 side) {
    std::vector<u8> img(size_t(side) * side * 4);
    for (u32 y = 0; y < side; ++y)
        for (u32 x = 0; x < side; ++x) {
            u8* p = &img[(size_t(y) * side + x) * 4];
            p[0] = uint8_t(x * 255 / (side - 1));
            p[1] = uint8_t(y * 255 / (side - 1));
            p[2] = uint8_t((x + y) * 255 / (2 * side - 2));
            p[3] = 255;
        }
    return img;
}

// 采样 pass 所需的 GPU 资源集合
struct sample_pass {
    ShaderHandle shader = 0;
    PipelineHandle pipeline = 0;
    PipelineLayoutHandle layout = 0;
    DescriptorSetLayoutHandle dsLayout = 0;
    DescriptorSetHandle ds = 0;
    SamplerHandle sampler = 0;
    ResourceHandle outBuf = 0;
    ResourceHandle outReadback = 0;
    ResourceHandle paramsBuf = 0;
    bool firstBarrier_ = true;
    static constexpr u32 kOutW = 256;

    bool build(RHIDeviceBase* dev, ResourceHandle pageTable, ResourceHandle atlas,
               ResourceHandle feedback) {
        auto cs = ReadSPV("Assets/Shaders/VTSys_TestSample.comp.spv");
        if (cs.empty()) {
            std::cerr << "missing VTSys_TestSample.comp.spv (run build_spv.sh)" << std::endl;
            return false;
        }
        shader = dev->CreateShader(cs.data(), cs.size(), ShaderStage::Compute, "main");
        if (shader == handles::INVALID_SHADER) { std::cerr << "[pass] CreateShader failed" << std::endl; return false; }

        SamplerDesc sdesc{};
        sdesc.minFilter = FilterMode::Nearest;
        sdesc.magFilter = FilterMode::Nearest;
        sdesc.addressU = TextureAddressMode::Clamp;
        sdesc.addressV = TextureAddressMode::Clamp;
        sampler = dev->CreateSampler(sdesc);
        if (sampler == handles::INVALID_SAMPLER) { std::cerr << "[pass] CreateSampler failed" << std::endl; return false; }

        DescriptorSetLayoutBinding b[6]{};
        b[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        b[2] = {2, DescriptorType::Sampler, 1, ShaderStage::Compute, nullptr};
        b[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};
        b[4] = {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};
        b[5] = {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};
        DescriptorSetLayoutDesc dsl{};
        dsl.bindingCount = 6;
        dsl.bindings = b;
        dsLayout = dev->CreateDescriptorSetLayout(dsl);
        PipelineLayoutDesc pl{};
        pl.setLayoutCount = 1;
        pl.setLayouts = &dsLayout;
        pl.pushConstantRangeCount = 0;
        layout = dev->CreatePipelineLayout(pl);
        ComputePipelineDesc cpd{};
        cpd.computeShader = shader;
        cpd.layout = layout;
        cpd.threadGroupSize = {8, 8, 1};
        pipeline = dev->CreateComputePipeline(cpd);

        DescriptorSetDesc dsd{};
        dsd.layout = dsLayout;
        ds = dev->CreateDescriptorSet(dsd);

        BufferDesc od{};
        od.size = u64(kOutW) * kOutW * 4;
        od.type = BufferType::Raw;
        od.memoryUsage = GPUMemoryUsage::Static;
        od.name = "VTSysTest_Out";
        outBuf = dev->CreateBuffer(od);
        BufferDesc rd = od;
        rd.memoryUsage = GPUMemoryUsage::Readback;
        rd.name = "VTSysTest_OutReadback";
        outReadback = dev->CreateBuffer(rd);
        BufferDesc pd{};
        pd.size = 48;
        pd.type = BufferType::Constant;
        pd.memoryUsage = GPUMemoryUsage::Dynamic;
        pd.name = "VTSysTest_Params";
        paramsBuf = dev->CreateBuffer(pd);
        if (pipeline == handles::INVALID_PIPELINE || ds == handles::INVALID_DESCRIPTOR_SET ||
            outBuf == handles::INVALID_RESOURCE || outReadback == handles::INVALID_RESOURCE ||
            paramsBuf == handles::INVALID_RESOURCE) {
            std::cerr << "[pass] create failed" << std::endl;
            return false;
        }

        DescriptorImageInfo ptInfo{}, atInfo{};
        ptInfo.imageView = pageTable;
        ptInfo.imageLayout = ResourceState::ShaderResource;
        atInfo.imageView = atlas;
        atInfo.imageLayout = ResourceState::ShaderResource;
        DescriptorImageInfo sampInfo{};
        sampInfo.sampler = sampler;
        DescriptorBufferInfo pInfo{paramsBuf, 0, 48};
        DescriptorBufferInfo oInfo{outBuf, 0, 0};
        DescriptorBufferInfo fInfo{feedback, 0, 0};
        WriteDescriptorSet w[6]{};
        for (u32 i = 0; i < 6; ++i) {
            w[i].dstSet = ds;
            w[i].dstBinding = i;
            w[i].dstArrayElement = 0;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = DescriptorType::SampledImage;   w[0].imageInfo = &ptInfo;
        w[1].descriptorType = DescriptorType::SampledImage;   w[1].imageInfo = &atInfo;
        w[2].descriptorType = DescriptorType::Sampler;        w[2].imageInfo = &sampInfo;
        w[3].descriptorType = DescriptorType::UniformBuffer;  w[3].bufferInfo = &pInfo;
        w[4].descriptorType = DescriptorType::StorageBuffer;  w[4].bufferInfo = &oInfo;
        w[5].descriptorType = DescriptorType::StorageBuffer;  w[5].bufferInfo = &fInfo;
        dev->UpdateDescriptorSets(6, w);
        std::cout << "[pass] built OK" << std::endl;
        return true;
    }

    // 参数:3 × vec4
    struct params3 {
        float p0[4];   // vtId, ptOriginX, ptOriginY, pagesTop
        float p1[4];   // level, outWidth, uvScale, uvOffsetX
        float p2[4];   // uvOffsetY, atlasSizeInv, avgColor, tileSize
    };

    bool dispatch_frame(RHIDeviceBase* dev, virtual_texture_system& sys,
                        u32 vt_id, u32 frame, float uvScale, float offX, float offY,
                        u32 atlasSize) {
        params3 p{};
        p.p0[0] = float(vt_id);
        p.p0[1] = float(sys.vt_pt_origin_x(vt_id));
        p.p0[2] = float(sys.vt_pt_origin_y(vt_id));
        p.p0[3] = float(sys.vt_pages_top(vt_id));
        p.p1[0] = 0;                       // level 0
        p.p1[1] = float(kOutW);
        p.p1[2] = uvScale;
        p.p1[3] = offX;
        p.p2[0] = offY;
        p.p2[1] = 1.0f / float(atlasSize);
        p.p2[2] = float(sys.vt_avg_color(vt_id));
        p.p2[3] = 128.0f;
        if (paramsBuf == handles::INVALID_RESOURCE ||
            !dev->UpdateBufferData(paramsBuf, &p, sizeof(p), 0)) return false;

        // feedback 绑定必须指向本帧槽 — 重建 b5 写
        {
            DescriptorBufferInfo fInfo{sys.feedback_buffer(frame), 0, 0};
            WriteDescriptorSet w{};
            w.dstSet = ds; w.dstBinding = 5; w.dstArrayElement = 0;
            w.descriptorCount = 1;
            w.descriptorType = DescriptorType::StorageBuffer;
            w.bufferInfo = &fInfo;
            dev->UpdateDescriptorSets(1, &w);
        }

        CommandBufferHandle cmd = dev->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* cb = GetCommandBuffer(cmd);
        if (!cb) return false;
        cb->Reset();
        cb->Begin();
        if (firstBarrier_) {   // 首帧:页表/atlas 新建(UNDEFINED)→ 采样布局
            firstBarrier_ = false;
            ResourceBarrier initB[2]{};
            initB[0].resource = sys.page_table_texture();
            initB[0].beforeState = ResourceState::Unknown;
            initB[0].afterState = ResourceState::ShaderResource;
            initB[0].subresource = 0xFFFFFFFF;
            initB[0].queueFamily = 0xFFFFFFFF;
            initB[1].resource = sys.atlas_texture(0);
            initB[1].beforeState = ResourceState::Unknown;
            initB[1].afterState = ResourceState::ShaderResource;
            initB[1].subresource = 0xFFFFFFFF;
            initB[1].queueFamily = 0xFFFFFFFF;
            cb->InsertBarrier(initB, 2);
        }
        cb->BindComputePipeline(pipeline);
        cb->BindDescriptorSets(PipelineBindPoint::Compute, layout, 0, 1, &ds, 0, nullptr);
        cb->Dispatch(kOutW / 8, kOutW / 8, 1);
        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        sys.record_feedback_copy(cb, frame, si);
        cb->CopyBuffer(outBuf, outReadback, 0, 0, u64(kOutW) * kOutW * 4);
        cb->End();
        if (!dev->Submit(si)) return false;
        if (!sys.wait_feedback(frame, 5000)) return false;   // fence 后再 free(M6 异步化)
        dev->DestroyCommandBuffer(cmd);
        return true;
    }

    double read_psnr(RHIDeviceBase* dev, const std::vector<u8>& ref, u32 side) {
        CommandBufferHandle drain = dev->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* waitCmd = GetCommandBuffer(drain);
        waitCmd->Reset(); waitCmd->Begin(); waitCmd->End();
        waitCmd->Submit(0);
        waitCmd->WaitForCompletion();   // 排空此前提交,保证 outReadback 可读
        dev->DestroyCommandBuffer(drain);
        const u32* mapped = static_cast<const u32*>(dev->MapBuffer(outReadback, 0, 0));
        if (!mapped) return -1.0;
        double sse = 0;
        const float scale = float(side) / kOutW;
        for (u32 y = 0; y < kOutW; ++y)
            for (u32 x = 0; x < kOutW; ++x) {
                u32 got = mapped[size_t(y) * kOutW + x];
                u32 rx = u32(x * scale), ry = u32(y * scale);
                rx = std::min(rx, side - 1); ry = std::min(ry, side - 1);
                const u8* e = &ref[(size_t(ry) * side + rx) * 4];
                u32 exp_ = u32(e[0]) | (u32(e[1]) << 8) | (u32(e[2]) << 16) | 0xFF000000u;
                for (int c = 0; c < 3; ++c) {
                    double d = double((got >> (8 * c)) & 0xFF) - double((exp_ >> (8 * c)) & 0xFF);
                    sse += d * d;
                }
            }
        dev->UnmapBuffer(outReadback);
        double mse = sse / (double(kOutW) * kOutW * 3);
        return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }

    void destroy(RHIDeviceBase* dev) {
        dev->DestroyBuffer(outBuf); dev->DestroyBuffer(outReadback);
        dev->DestroyBuffer(paramsBuf); dev->DestroySampler(sampler);
        dev->DestroyDescriptorSet(ds); dev->DestroyDescriptorSetLayout(dsLayout);
        dev->DestroyPipelineLayout(layout); dev->DestroyPipeline(pipeline);
        dev->DestroyShader(shader);
    }
};

} // anonymous namespace

TestResult TestVTSVT_RuntimeLoop() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    auto& sys = virtual_texture_system::instance();

    // ================= Phase 1:正确性 =================
    {
        vt_config cfg;
        cfg.feedback_slots = sample_pass::kOutW * sample_pass::kOutW;
        cfg.atlas_size = 2048;
        TEST_ASSERT(sys.initialize(fx.base, cfg), "system initialize");

        std::vector<u8> src = make_source(256);
        std::vector<u8> vtFile;
        TEST_ASSERT(content::vt::bake(src.data(), 256, 256, content::vt::bake_config{}, vtFile),
                    "bake 256");
        u32 vt = sys.register_texture(vtFile.data(), vtFile.size());
        TEST_ASSERT(vt != ~0u, "register texture");
        TEST_ASSERT_EQ(2u, sys.vt_pages_top(vt), "pages_top");

        sample_pass pass;
        TEST_ASSERT(pass.build(fx.base, sys.page_table_texture(),
                               sys.atlas_texture(0), sys.feedback_buffer(0)),
                    "build sample pass");

        for (u32 frame = 0; frame < 10; ++frame) {
            TEST_ASSERT(pass.dispatch_frame(fx.base, sys, vt, frame, 1.0f, 0, 0, 2048),
                        "dispatch frame");
            sys.update(frame);
        }
        // 容忍 worker 异步:多跑几帧空 update 收尾
        for (u32 i = 0; i < 4; ++i) sys.update(100 + i);

        const vt_stats& st = sys.stats();
        std::cout << "[VTSys] resident=" << st.resident_pages
                  << " feedback=" << st.feedback_processed
                  << " poolUsed=" << st.pool_pages_used << "/" << st.pool_pages_total
                  << " evict=" << st.evictions_total << std::endl;
        TEST_ASSERT_EQ(5u, st.resident_pages, "4 mip0 pages + root resident");
        TEST_ASSERT(st.feedback_processed > 0, "feedback processed");
        TEST_ASSERT_EQ(0u, st.evictions_total, "no eviction phase1");
        TEST_ASSERT_EQ(5u, st.pool_pages_used, "pool used = 5");

        double psnr = pass.read_psnr(fx.base, src, 256);
        std::cout << "[VTSys] phase1 PSNR = " << psnr << " dB" << std::endl;
        TEST_ASSERT(psnr >= 28.0, "sampled output PSNR >= 28dB");

        pass.destroy(fx.base);
        sys.release_texture(vt);
        sys.shutdown();
        TEST_ASSERT_EQ(0u, sys.stats().registered, "released");
    }

    // ================= Phase 2:淘汰(小页池 3x3=9 槽 vs 21 页) =================
    {
        vt_config cfg;
        cfg.feedback_slots = sample_pass::kOutW * sample_pass::kOutW;
        cfg.atlas_size = 512;                 // 512/136 = 3 → 9 页容量
        cfg.max_uploads_per_frame = 4;
        TEST_ASSERT(sys.initialize(fx.base, cfg), "system initialize phase2");

        std::vector<u8> src = make_source(512);
        std::vector<u8> vtFile;
        TEST_ASSERT(content::vt::bake(src.data(), 512, 512, content::vt::bake_config{}, vtFile),
                    "bake 512");
        u32 vt = sys.register_texture(vtFile.data(), vtFile.size());
        TEST_ASSERT(vt != ~0u, "register 512");
        TEST_ASSERT_EQ(4u, sys.vt_pages_top(vt), "pages_top 4");
        // 总页数 16+4+1 = 21 > 9 容量 → 强制淘汰路径

        sample_pass pass;
        TEST_ASSERT(pass.build(fx.base, sys.page_table_texture(),
                               sys.atlas_texture(0), sys.feedback_buffer(0)),
                    "build phase2 pass");

        const float off[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};
        u32 frame = 0;
        bool capacityOk = true;
        for (u32 round = 0; round < 6; ++round)
            for (int q = 0; q < 4; ++q, ++frame) {
                TEST_ASSERT(pass.dispatch_frame(fx.base, sys, vt, frame, 0.5f,
                                                 off[q][0], off[q][1], 512),
                            "dispatch quadrant");
                sys.update(frame);
                if (sys.stats().pool_pages_used > sys.stats().pool_pages_total ||
                    sys.stats().pool_pages_total != 9)
                    capacityOk = false;
            }
        for (u32 i = 0; i < 6; ++i) sys.update(500 + i);

        const vt_stats& st = sys.stats();
        std::cout << "[VTSys] phase2: resident=" << st.resident_pages
                  << " pool=" << st.pool_pages_used << "/" << st.pool_pages_total
                  << " evict=" << st.evictions_total << std::endl;
        TEST_ASSERT(capacityOk, "pool occupancy within capacity");
        TEST_ASSERT_EQ(9u, st.pool_pages_total, "small pool capacity");
        TEST_ASSERT(st.evictions_total > 0, "evictions happened");
        TEST_ASSERT(st.resident_pages >= 1, "root stays resident");

        pass.destroy(fx.base);
        sys.release_texture(vt);
        sys.shutdown();
    }
    return TestResult::Passed;
}

// ============================================================================
// M2.17 压测:注册/采样/释放循环 — 页池与流送队列计数归零(泄漏审计)
// ============================================================================
TestResult TestVTSVT_RegisterReleaseStress() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    auto& sys = virtual_texture_system::instance();

    vt_config cfg;
    cfg.feedback_slots = 256 * 256;
    cfg.atlas_size = 1024;   // 7x7 = 49 页容量
    TEST_ASSERT(sys.initialize(fx.base, cfg), "system initialize");

    std::vector<u8> src = make_source(256);
    std::vector<u8> vtFile;
    TEST_ASSERT(content::vt::bake(src.data(), 256, 256, content::vt::bake_config{}, vtFile),
                "bake stress");

    for (u32 round = 0; round < 20; ++round) {
        u32 vt = sys.register_texture(vtFile.data(), vtFile.size());
        TEST_ASSERT(vt != ~0u, "register round");
        sample_pass pass;
        TEST_ASSERT(pass.build(fx.base, sys.page_table_texture(),
                               sys.atlas_texture(0), sys.feedback_buffer(0)),
                    "build stress pass");
        for (u32 frame = 0; frame < 3; ++frame) {
            TEST_ASSERT(pass.dispatch_frame(fx.base, sys, vt, frame, 1.0f, 0, 0, 1024),
                        "dispatch stress");
            sys.update(frame);
        }
        for (u32 i = 0; i < 3; ++i) sys.update(900 + i);
        pass.destroy(fx.base);
        sys.release_texture(vt);
    }
    // 收尾:排干 worker + 统计
    for (u32 i = 0; i < 8; ++i) sys.update(2000 + i);
    const vt_stats& st = sys.stats();
    std::cout << "[VTSys] stress: registered=" << st.registered
              << " poolUsed=" << st.pool_pages_used << "/" << st.pool_pages_total
              << " pending=" << st.pending_requests
              << " evict=" << st.evictions_total << std::endl;
    TEST_ASSERT_EQ(0u, st.registered, "all released");
    TEST_ASSERT_EQ(0u, st.pool_pages_used, "pool fully freed");
    TEST_ASSERT_EQ(0u, st.pending_requests, "streaming queue drained");
    sys.shutdown();
    return TestResult::Passed;
}

void RegisterVTSVTRuntimeTests() {
    auto suite = std::make_shared<TestSuite>("VTSVTRuntimeTests");
    suite->AddTestCase(TestCase("RuntimeLoop", TestVTSVT_RuntimeLoop));
    suite->AddTestCase(TestCase("RegisterReleaseStress", TestVTSVT_RegisterReleaseStress));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVTSVTRuntimeTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVTSVTRuntime] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
