/**
 * @file TestSponzaVT.cpp
 * @brief VT M2.16 — Sponza 全材质 VT 化集成测试(Vulkan/MoltenVK)
 * @details 走 ForwardSceneRenderer 真实 mesh 路径:
 *   EnableVirtualTextures(vtex 目录) → LoadScene(Sponza.model) → N 帧
 *   Render(内含 GBufferVT 管线按材质切换)→ AttachVTFeedback + submit →
 *   virtual_texture_system::update 驱动流送。
 *
 * 验收:
 *   - VT 材质数 > 0(mesh 路径接线生效)
 *   - feedback 处理量 > 0(片元请求进入流送)
 *   - 驻留页 > 0 且稳定收敛(tile 实际上传)
 *   - 多帧渲染稳定无崩溃(VT/非 VT 材质混跑)
 *
 * vtex 资产:优先用预烘焙目录 /tmp/sponza_vt(75 张全量,scripts/vt/
 * bake_sponza.sh 产物);不存在时测试内烘焙 4 张代表纹理(子集覆盖,
 * 其余材质自动回退常规路径 — 严格模式语义)。
 */

#include "../UnitTests/TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderPipeline/Modules/ForwardSceneRenderer.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Components/Transform.h"
#include "Graphics/VirtualTexture/VirtualTextureSystem.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "VTBaker.h"

using namespace primal;
using namespace primal::graphics;
using namespace Engine::Test;

namespace {

const char* kPrebakedDir = "/tmp/sponza_vt";

// 测试内烘焙子集(每张 ~1-3s)
bool BakeSubset(const std::string& outDir) {
    namespace fs = std::filesystem;
    fs::create_directories(outDir);
    const char* names[4][2] = {
        {"Sponza_Arch_diffuse.png", "color"},
        {"Sponza_Arch_normal.png", "normal"},
        {"Sponza_Arch_roughness.png", "scalar"},
        {"Sponza_Ceiling_diffuse.png", "color"},
    };
    for (auto& [png, kind] : names) {
        std::string src = "../../../EngineTest/assets/models/Sponza/" + std::string(png);
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load(src.c_str(), &w, &h, &comp, 4);
        if (!px) {
            std::cerr << "[SponzaVT] skip " << png << " (asset not found)" << std::endl;
            continue;
        }
        content::vt::bake_config cfg;
        if (std::string(kind) == "normal") cfg.kind = content::vt::channel_kind::normal;
        else if (std::string(kind) == "scalar") cfg.kind = content::vt::channel_kind::scalar;
        std::vector<u8> out;
        if (!content::vt::bake(px, u32(w), u32(h), cfg, out)) {
            stbi_image_free(px);
            return false;
        }
        stbi_image_free(px);
        std::string stem = std::string(png);
        stem = stem.substr(0, stem.rfind('.'));
        std::ofstream f(outDir + "/" + stem + ".vtex", std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    }
    return true;
}

struct DeviceFixture {
    rhi::RHIDeviceBase* base{nullptr};
    rhi::DeviceDesc desc{};
    bool Init() {
        desc.platform = rhi::RHIPlatform::Vulkan;
        desc.enableValidation = false;   // 集成路径长,validation 关闭保速度
        desc.enableDebug = true;
        base = rhi::CreateRHIDevice(desc);
        return base != nullptr;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

} // anonymous namespace

TestResult TestSponzaVT_Integration() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // vtex 资产:预烘焙目录优先
    std::string vtexDir = kPrebakedDir;
    {
        std::error_code ec;
        size_t n = 0;
        if (std::filesystem::exists(vtexDir, ec))
            for (auto& e : std::filesystem::directory_iterator(vtexDir, ec))
                if (e.path().extension() == ".vtex") ++n;
        if (n < 10) {
            vtexDir = "/tmp/sponza_vt_subset";
            std::cout << "[SponzaVT] 预烘焙目录不可用,测试内烘焙子集 → " << vtexDir << std::endl;
            TEST_ASSERT(BakeSubset(vtexDir), "bake subset");
        } else {
            std::cout << "[SponzaVT] 使用预烘焙目录(" << n << " 张)" << std::endl;
        }
    }

    // StandardRenderPipeline(编辑模式 = ForwardSceneRenderer 直连路径 —
    // FSR-VK-GAP 修复验证:全场景 Vulkan 渲染)
    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "pipeline initialize");
    pipeline.SetEditorMode(true);
    {
        auto loadSpv = [&](const char* rel) -> rhi::ShaderHandle {
            std::ifstream f(rel, std::ios::binary | std::ios::ate);
            if (!f) return rhi::handles::INVALID_SHADER;
            auto sz = f.tellg();
            f.seekg(0);
            std::vector<char> b(size_t(sz), 0);
            f.read(b.data(), sz);
            return fx.base->CreateShader(b.data(), b.size(),
                                         std::string(rel).find(".vert.spv") != std::string::npos
                                             ? rhi::ShaderStage::Vertex
                                             : rhi::ShaderStage::Pixel,
                                         "main");
        };
        StandardRenderPipeline::ShaderHandles handles;
        handles.deferred_vs = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.vert.spv");
        handles.deferred_ps = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.frag.spv");
        handles.blit_vs = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.vert.spv");
        handles.blit_ps = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.frag.spv");
        handles.shadow_filter = loadSpv("Engine/Graphics/Vulkan/shaders/ShadowFilter.spv");
        pipeline.SetShaderHandles(handles);
        pipeline.SetLumenConfig({});   // 触发 InitializeSubsystems(forward_renderer_)
    }
    ForwardSceneRenderer* fr = pipeline.GetForwardRenderer();
    TEST_ASSERT(fr != nullptr, "forward renderer");
    fr->EnableVirtualTextures(vtexDir);
    TEST_ASSERT(fr->IsVirtualTextureEnabled(), "VT mode enabled");

    // 真实 Sponza 全场景(393 代理 — FSR-VK-GAP 崩溃复现规模)
    TEST_ASSERT(pipeline.LoadEditorScene("EngineTest/assets/Sponza.model"), "LoadEditorScene");
    const u32 vtCount = fr->VTMaterialCount();
    std::cout << "[SponzaVT] VT materials: " << vtCount << std::endl;
    TEST_ASSERT(vtCount > 0, "VT materials created (mesh path)");

    // ECS 桥
    auto statics = fr->CreateStaticEntities();
    graphics::RenderScene scene;
    for (size_t i = 0; i < statics.entity_ids.size(); ++i) {
        const id::id_type eid = statics.entity_ids[i];
        primal::math::m4x4 world, invWorld;
        primal::transform::get_transform_matrices(game_entity::entity_id{eid}, world, invWorld);
        const auto* mi = fr->GetMeshInfo(statics.mesh_slot_indices[i]);
        if (!mi || !mi->mesh) continue;
        auto proxy = graphics::RenderProxy::Create(
            eid, mi->mesh->GetEntityId(), mi->gpuMaterialId);
        proxy.UpdateTransform(world);
        scene.AddProxy(proxy);
    }
    fr->SetRenderScene(&scene);
    {
        graphics::RenderLight sun{};
        sun.type = graphics::LightType::Directional;
        sun.color = primal::math::v3{1.0f, 0.96f, 0.9f};
        sun.intensity = 3.0f;
        scene.AddLight(sun);
    }
    graphics::RenderView view;
    {
        namespace rhimath = primal::graphics::rhi::math;
        view.SetViewMatrix(rhimath::CreateLookAtMatrix(
            rhimath::v3{0.0f, 2.0f, -8.0f}, rhimath::v3{0.0f, 2.0f, 0.0f},
            rhimath::v3{0.0f, 1.0f, 0.0f}));
        view.SetProjectionMatrix(rhimath::CreatePerspectiveMatrix(
            60.0f * rhimath::constants::DEG_TO_RAD, 1024.0f / 768.0f, 0.1f, 1000.0f));
        view.SetViewport({{0, 0}, {1024.0f, 768.0f}, 0, 1});
        view.SetScissor({{0, 0}, {1024, 768}});
        view.UpdateFrustum();
        view.Cull(scene);
    }

    // 输出 RT
    rhi::TextureDesc outDesc{};
    outDesc.size = {1024, 768, 1};
    outDesc.mipLevels = 1;
    outDesc.arraySize = 1;
    outDesc.format = rhi::DataFormat::BGRA8_UNorm;
    outDesc.type = rhi::TextureType::Texture2D;
    outDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
    outDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    outDesc.name = "SponzaVT_Output";
    rhi::ResourceHandle output = fx.base->CreateTexture(outDesc);
    TEST_ASSERT(output != rhi::handles::INVALID_RESOURCE, "output RT");

    // 帧循环:RenderWithCommandBuffer → feedback 拷贝 → Submit(ring fence)
    // → wait → update
    auto& sys = vt::virtual_texture_system::instance();
    const u32 kFrames = 24;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        rhi::CommandBufferHandle cmd = fx.base->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        rhi::RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
        TEST_ASSERT(cb && cb->Reset() && cb->Begin(), "begin frame");
        pipeline.RenderWithCommandBuffer(scene, view, output, outDesc, cb, 0, cmd,
                                          rhi::handles::INVALID_SYNC);
        rhi::QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        si.signalFence = sys.feedback_fence_handle(frame);
        sys.record_feedback(cb, frame);
        TEST_ASSERT(cb->End(), "end frame");
        TEST_ASSERT(fx.base->Submit(si), "submit frame");
        TEST_ASSERT(sys.wait_feedback(frame, 5000), "feedback fence");
        fx.base->DestroyCommandBuffer(cmd);
        sys.update(frame);
    }

    // 场景级闭环统计(FSR-VK-GAP 修复后应全过)
    {
        // 排空 GPU 后读输出画面
        CommandBufferHandle drain = fx.base->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        RHICommandBuffer* wc = rhi::GetCommandBuffer(drain);
        wc->Reset(); wc->Begin(); wc->End();
        wc->Submit(0);
        wc->WaitForCompletion();
        fx.base->DestroyCommandBuffer(drain);
    }
    {
        rhi::BufferDesc rb{};
        rb.size = 1024ull * 768 * 4;
        rb.type = rhi::BufferType::Raw;
        rb.memoryUsage = rhi::GPUMemoryUsage::Readback;
        rhi::ResourceHandle rbBuf = fx.base->CreateBuffer(rb);
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        auto* cb = rhi::GetCommandBuffer(cmd);
        cb->Reset(); cb->Begin();
        rhi::BufferTextureCopyRegion reg{};
        reg.imageSubresource.layerCount = 1;
        reg.imageExtent = {1024, 768, 1};
        cb->CopyTextureToBuffer(output, rbBuf, &reg, 1);
        cb->End();
        cb->Submit(0);
        cb->WaitForCompletion();
        fx.base->DestroyCommandBuffer(cmd);
        const u8* m = static_cast<const u8*>(fx.base->MapBuffer(rbBuf, 0, 0));
        u64 nonzero = 0;
        for (u64 i = 0; i < rb.size; ++i) if (m[i]) ++nonzero;
        fx.base->UnmapBuffer(rbBuf);
        fx.base->DestroyBuffer(rbBuf);
        std::cout << "[SponzaVT] output non-zero bytes: " << nonzero << "/" << rb.size << std::endl;
        TEST_ASSERT(nonzero > rb.size / 10, "scene visibly rendered");
    }

    const vt::vt_stats& st = sys.stats();
    std::cout << "[SponzaVT] stats: feedback=" << st.feedback_processed
              << " resident=" << st.resident_pages
              << " pool=" << st.pool_pages_used << "/" << st.pool_pages_total
              << " evict=" << st.evictions_total << std::endl;
    TEST_ASSERT(st.feedback_processed > 0, "fragment feedback processed (scene loop)");
    TEST_ASSERT(st.resident_pages > 3, "pages streamed beyond roots");
    TEST_ASSERT(st.pool_pages_used > 0 && st.pool_pages_used <= st.pool_pages_total,
                "pool occupancy sane");

    fx.base->DestroyTexture(output);
    pipeline.Shutdown();
    sys.shutdown();
    return TestResult::Passed;
}

void RegisterSponzaVTTests() {
    auto suite = std::make_shared<TestSuite>("SponzaVTTests");
    suite->AddTestCase(TestCase("Integration", TestSponzaVT_Integration));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterSponzaVTTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "[TestSponzaVT] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
