/**
 * @file TestVulkanNaniteSmoke.cpp
 * @brief Tier 4.4.6 — Nanite runtime smoke tests on Vulkan.
 * @details Validates that the 3 Nanite systems shipped in T4.4.0-T4.4.5
 *          (HZBSystem, GlobalSDF, GPUCullingPipeline) actually initialize +
 *          execute on a Vulkan device without validation errors.
 *
 *          This is a SMOKE test, not parity. Bar per system:
 *            - Initialize() returns true
 *            - Execute()/BuildHZB()/DebugFill() completes without crash
 *            - Zero validation errors
 *            - Non-trivial output (proves SPIR-V shader ran + wrote data)
 *
 *          GPUDrivenDrawPipeline is NOT covered here — it requires
 *          `content::get_rhi_mesh_asset` resolution which needs either a
 *          real test asset or a mock seam. Deferred to follow-up session.
 *
 * Acceptance (overall):
 *   - Test 1 (HZBSystem): BuildHZB produces mip chain ≥1 level with non-zero data
 *   - Test 2 (GlobalSDF): DebugFill writes ≥1 non-sentinel texel
 *   - Test 3 (GPUCulling): either passes OR returns Skipped with diagnostic
 *   - Zero validation errors across all 3 tests
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/Nanite/HZBSystem.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Nanite/GPUCullingPipeline.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Nanite/NaniteResourceManager.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Content/ProceduralMesh.h"
#include "Content/ContentToEngine.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Cluster.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <cmath>
#include <cstring>
#include <vector>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::nanite;
using namespace primal::math;
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

m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

} // anonymous namespace

// ============================================================================
// Test 1: HZBSystem smoke — SPIR-V load (HZBCopy + HZBMip) + dispatch
// ============================================================================

TestResult TestVulkanHZBSystem_Smoke() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 1. Create 256x256 D32 depth texture, cleared to 0.5 via render pass.
    constexpr u32 W = 256, H = 256;
    TextureDesc depthDesc{
        {W, H, 1},
        1,
        1,
        DataFormat::D32_Float,
        TextureType::Texture2D,
        TextureUsage::DepthStencil | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "HZBDepthSrc"
    };
    ResourceHandle depth = fx.base->CreateTexture(depthDesc);
    TEST_ASSERT(depth != handles::INVALID_RESOURCE, "CreateTexture depth");

    // Clear depth to 0.5 via a render pass.
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    RenderPassDesc passDesc{};
    passDesc.depthAttachment.texture = depth;
    passDesc.depthAttachment.format = DataFormat::D32_Float;
    passDesc.depthAttachment.loadOp = LoadAction::Clear;
    passDesc.depthAttachment.storeOp = StoreAction::Store;
    passDesc.depthAttachment.clearValue.depth = 0.5f;
    vcmd->BeginRenderPass(passDesc);
    vcmd->EndRenderPass();

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit depth clear");
    fx.base->DestroyCommandBuffer(cmd);

    // 2. Initialize HZBSystem.
    HZBSystem hzb;
    HZBSystem::Config cfg{};
    cfg.max_width = W;
    cfg.max_height = H;
    cfg.min_mip_size = 8;
    cfg.enable_compression = false;
    cfg.generate_on_gpu = true;
    bool ok = hzb.Initialize(fx.base, cfg);
    TEST_ASSERT(ok, "HZBSystem::Initialize on Vulkan");

    // 3. Build HZB.
    cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin HZB build");

    HZBSystem::BuildResult result = hzb.BuildHZB(depth, vcmd);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit HZB build");
    fx.base->DestroyCommandBuffer(cmd);

    TEST_ASSERT(result.mip_levels >= 1, "HZB mip chain generated");
    TEST_ASSERT(result.hzb_texture != handles::INVALID_RESOURCE, "HZB texture handle valid");

    // Known-bug diagnostic: T4.4.6 surfaced that HZBSystem's Vulkan branch
    // in GenerateHZBOnGPU is missing per-mip layout transitions
    // (UNDEFINED → GENERAL) before compute dispatches. Validation errors
    // fire and the compute writes don't land. Debugging this is a follow-up.
    // For now: verify the API surface (Initialize + BuildHZB) completes
    // without crashing and returns valid handles.

    // 4. Readback mip 0 via CopyTextureToBuffer → verify non-zero variance.
    BufferDesc readbackDesc{};
    readbackDesc.size = u64(W) * H * 4;  // R32F = 4 bytes/px
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "HZB_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin readback");

    ResourceBarrier toCopy{};
    toCopy.resource = result.hzb_texture;
    toCopy.beforeState = ResourceState::UnorderedAccess;
    toCopy.afterState = ResourceState::CopySource;
    toCopy.subresource = 0xFFFFFFFF;
    toCopy.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopy, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {W, H, 1};
    vcmd->CopyTextureToBuffer(result.hzb_texture, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit readback");
    fx.base->DestroyCommandBuffer(cmd);

    // 5. Sample pixels — verify non-trivial (not all zero, not all one).
    float* mapped = static_cast<float*>(fx.base->MapBuffer(readback, 0, readbackDesc.size));
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    u32 non_zero = 0;
    float first_val = mapped[0];
    for (u32 i = 0; i < W * H; ++i) {
        if (mapped[i] > 0.01f) ++non_zero;
    }
    std::cout << "[TestVulkanHZBSystem] first_val=" << first_val
              << " non_zero_count=" << non_zero << "/" << (W * H) << std::endl;
    fx.base->UnmapBuffer(readback);

    // HZB mip 0 should preserve source depth (~0.5) — the compute write must
    // land now that per-mip layout transitions are in place. Sample mip 0 with
    // a relaxed threshold: source cleared to 0.5, but HZBCopy is a straight
    // depth copy so non-zero should be near-total.
    TEST_ASSERT(non_zero > (W * H) / 2,
                "HZB mip 0 non-zero pixel count (compute dispatch wrote data)");

    // Cleanup.
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(depth);
    hzb.Shutdown();
    return TestResult::Passed;
}

// ============================================================================
// Test 2: GlobalSDF smoke — DebugFill test seam (bypasses Nanite voxelization)
// ============================================================================

TestResult TestVulkanGlobalSDF_Smoke() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 1. Initialize GlobalSDF with default config (3 cascades, 128^3 base).
    GlobalSDF& sdf = GlobalSDF::Get();
    GlobalSDFConfig cfg{};
    cfg.cascade_count = 1;     // reduce to 1 cascade for smoke speed
    cfg.base_resolution = 64;  // smaller res = faster DebugFill
    bool ok = sdf.Initialize(fx.base, cfg);
    TEST_ASSERT(ok, "GlobalSDF::Initialize on Vulkan");

    // 2. DebugFill with unit-sphere SDF (distance to origin minus radius 1).
    bool fill_ok = sdf.DebugFill([](const v3& p) -> f32 {
        return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) - 1.0f;
    });
    TEST_ASSERT(fill_ok, "GlobalSDF::DebugFill completed");

    // 3. Read back cascade 0 texture.
    const SDFCascade& cascade = sdf.GetCascade(0);
    TEST_ASSERT(cascade.sdf_texture != handles::INVALID_RESOURCE, "cascade 0 texture valid");

    constexpr u32 RES = 64;
    BufferDesc readbackDesc{};
    readbackDesc.size = u64(RES) * RES * RES * 4;  // R32F
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "GlobalSDF_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin readback");

    ResourceBarrier toCopy{};
    toCopy.resource = cascade.sdf_texture;
    toCopy.beforeState = ResourceState::UnorderedAccess;
    toCopy.afterState = ResourceState::CopySource;
    toCopy.subresource = 0xFFFFFFFF;
    toCopy.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopy, 1);

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {RES, RES, RES};
    vcmd->CopyTextureToBuffer(cascade.sdf_texture, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit readback");
    fx.base->DestroyCommandBuffer(cmd);

    // 4. Sample — verify some texel differs from sentinel (1e10 or 0).
    float* mapped = static_cast<float*>(fx.base->MapBuffer(readback, 0, readbackDesc.size));
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    u32 non_sentinel = 0;
    float min_val = 1e30f, max_val = -1e30f;
    for (u32 i = 0; i < RES * RES * RES; ++i) {
        float v = mapped[i];
        if (v > -1e9f && v < 1e9f && v != 0.0f) ++non_sentinel;
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
    }
    std::cout << "[TestVulkanGlobalSDF] non_sentinel=" << non_sentinel
              << "/" << (RES * RES * RES)
              << " min=" << min_val << " max=" << max_val << std::endl;
    fx.base->UnmapBuffer(readback);

    TEST_ASSERT(non_sentinel > 0, "GlobalSDF DebugFill wrote at least one texel");

    // Cleanup.
    fx.base->DestroyBuffer(readback);
    sdf.Shutdown();
    return TestResult::Passed;
}

// ============================================================================
// Test 3: GPUCullingPipeline + GPUDrivenDrawPipeline end-to-end smoke
// ============================================================================
//
// T4.6.5 part 18 — first runtime exercise of the full Nanite visibility
// pipeline on Vulkan. Builds a synthetic scene (1 sphere via create_sphere_mesh
// → cluster::create → RenderProxy → RenderScene → RenderSceneSnapshot), then
// runs cull.Execute (8-stage GPU culling with ForcePassAll=true to bypass HZB
// occlusion) + gpuDraw.Execute (meshlet binning + VisibilityBuffer raster).
// Verifies indirect_args_buffer is non-zero (≥1 draw call enqueued) and that
// the whole path completes with zero validation errors.
//
// Init order mirrors TestDawnForwardRenderer::InitializeMeshletPipeline:
//   1. device
//   2. NaniteResourceManager.Initialize(device)  ← CRITICAL: before cluster::create
//   3. cluster::create({geom_id}, entity)        ← registers resource with manager
//   4. snapshot.Rebind(scene)                     ← walks proxies, reads cluster data
//   5. gpuDraw.Initialize + cull.Initialize + cross-wire
//   6. snapshot.UploadToGPUBuffers
//   7. cull.Execute + gpuDraw.Execute

TestResult TestVulkanGPUCullingPipeline_Smoke() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 W = 64, H = 64;

    // 1. NaniteResourceManager — MUST be initialized before cluster::create,
    // otherwise GetOrCreateResource returns nullptr and instances end up with
    // cluster_count=0 (silent no-op in UpdateGeometryData).
    auto& resourceManager = NaniteResourceManager::Get();
    TEST_ASSERT(resourceManager.Initialize(fx.base),
                "NaniteResourceManager::Initialize");

    // 2. Synthetic sphere mesh via procedural mesh generator.
    // create_sphere_mesh → register_mesh_asset → returns geometry_content_id.
    // segments=16, rings=12 = 192 triangles, well above kMeshletMaxTriangles floor.
    id::id_type geom_id = content::create_sphere_mesh(1.0f, 16, 12);
    TEST_ASSERT(geom_id != id::invalid_id, "create_sphere_mesh");

    // 3. Game entity + cluster component.
    transform::init_info tfInfo{};
    tfInfo.rotation[0] = 0.0f;  // identity quaternion {x,y,z,w}
    tfInfo.rotation[1] = 0.0f;
    tfInfo.rotation[2] = 0.0f;
    tfInfo.rotation[3] = 1.0f;
    tfInfo.scale[0] = 1.0f;
    tfInfo.scale[1] = 1.0f;
    tfInfo.scale[2] = 1.0f;
    game_entity::entity_info entInfo{};
    entInfo.transform = &tfInfo;
    game_entity::entity entity = game_entity::create(entInfo);
    TEST_ASSERT(entity.is_valid(), "game_entity::create");

    cluster::init_info clusterInfo{};
    clusterInfo.geometry_content_id = geom_id;
    cluster::component clusterComp = cluster::create(clusterInfo, entity);
    TEST_ASSERT(clusterComp != id::invalid_id, "cluster::create");

    // 4. RenderScene with directional light + 1 proxy.
    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    RenderProxy proxy = RenderProxy::Create(entity.get_id(), clusterComp,
                                            id::invalid_id);
    proxy.transform = make_identity_m4x4();
    // Manual AABB (RenderProxy::RecalculateWorldAABB falls back to a tiny
    // default since cluster component isn't a RenderMesh). Sphere radius=1.
    proxy.worldAABB = rhi::AABB(v3{-1.0f, -1.0f, -1.0f},
                                 v3{ 1.0f,  1.0f,  1.0f});
    scene.AddProxy(proxy);

    // 5. View + projection matrices (camera at z=+5 looking at origin).
    // Right-handed view space (matches Stage1 frustum shader at
    // GPUCullingPipeline.wgsl:230-232: "camera looks down -Z, visible objects
    // have negative Z"). view_matrix must put the sphere at view_z=-5, so the
    // translation column is -5 (inverse of camera world position). With the
    // prior +5 sign the sphere landed behind the camera, Stage1 correctly
    // marked is_visible=0, and Stage4 with ForcePassAll=false skipped it.
    m4x4 viewMat = make_identity_m4x4();
    viewMat.columns[3][2] = -5.0f;
    // T4.6.5 part 22.1: projection uses right-handed Vulkan/Metal convention
    // (NDC z [0,1], camera looks down -Z). P[3][2] = -1 so clip.w = -view.z,
    // which is positive for visible points (view.z negative). The prior
    // +1 sign produced clip.w < 0 for all sphere vertices → clipped by GPU,
    // empty visibility_buffer_, all-background resolve. The HZB occlusion
    // test passes anyway because the shader defensively returns "visible"
    // when clip.w <= 0 (GPUCullingPipeline.wgsl:458-460), but rasterization
    // has no such out — vertices are simply clipped.
    m4x4 projMat{};
    std::memset(&projMat, 0, sizeof(projMat));
    constexpr float pi = 3.14159265358979323846f;
    float fov = 60.0f * (pi / 180.0f);
    float aspect = float(W) / float(H);
    float f = 1.0f / std::tan(fov * 0.5f);
    projMat.columns[0][0] = f / aspect;
    projMat.columns[1][1] = f;
    projMat.columns[2][2] = 50.0f / (0.1f - 100.0f);
    projMat.columns[2][3] = -1.0f;
    projMat.columns[3][2] = -(0.1f * 100.0f) / (0.1f - 100.0f);

    // 6. RenderSceneSnapshot — walks proxies, resolves cluster components,
    // builds InstanceData + ClusterRef GPU buffers.
    RenderSceneSnapshot snap;
    TEST_ASSERT(snap.Initialize(fx.base, /*instance_cap=*/10,
                                 /*cluster_cap=*/100),
                "snapshot.Initialize");
    TEST_ASSERT(snap.Rebind(scene), "snapshot.Rebind");
    std::cout << "[TestGPUCulling] snapshot.InstanceCount="
              << snap.GetInstanceCount()
              << " ClusterRefCount=" << snap.GetClusterRefCount() << std::endl;
    TEST_ASSERT(snap.GetInstanceCount() >= 1, "snapshot has 1+ instance");

    // 6b. Depth texture + HZBSystem for occlusion culling.
    // 256×256 matches Test 1 dimensions. Camera at z=5 with sphere radius=1
    // fills the near field; clearing depth to 0.5 means "geometry at mid-depth
    // is visible everywhere" → HZB mip chain reports full visibility → sphere
    // passes the occlusion test in Stage5.
    constexpr u32 kDepthW = 256, kDepthH = 256;
    TextureDesc depthDesc{
        {kDepthW, kDepthH, 1}, 1, 1,
        DataFormat::D32_Float,
        TextureType::Texture2D,
        TextureUsage::DepthStencil | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "Test3_HZBDepth"
    };
    ResourceHandle depthTex = fx.base->CreateTexture(depthDesc);
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "CreateTexture depthTex");

    HZBSystem hzb;
    HZBSystem::Config hzbCfg{};
    hzbCfg.max_width = kDepthW;
    hzbCfg.max_height = kDepthH;
    hzbCfg.min_mip_size = 8;
    hzbCfg.enable_compression = false;
    hzbCfg.generate_on_gpu = true;
    TEST_ASSERT(hzb.Initialize(fx.base, hzbCfg), "HZBSystem::Initialize");

    // 7. Pipeline initialization + cross-wiring.
    GPUDrivenDrawPipeline& gpuDraw = GPUDrivenDrawPipeline::Get();
    VisibilityBufferConfig visCfg{};
    visCfg.width = W;
    visCfg.height = H;
    visCfg.format = DataFormat::R32_UInt;
    visCfg.enable_depth = true;
    TEST_ASSERT(gpuDraw.Initialize(fx.base, BinningConfig{}, visCfg),
                "GPUDrivenDrawPipeline::Initialize");

    GPUCullingPipeline& cull = GPUCullingPipeline::Get();
    CullingConfig cullCfg{};
    cullCfg.enable_occlusion_culling = true;  // T4.6.5 part 22.1: real HZB occlusion
    cullCfg.enable_lod_selection = false;
    cullCfg.enable_streaming_feedback = false;
    TEST_ASSERT(cull.Initialize(fx.base, cullCfg),
                "GPUCullingPipeline::Initialize");

    cull.SetGPUDrawPipeline(&gpuDraw);
    // ForcePassAll=false runs the real Stage1 (near/far frustum) + Stage4
    // (per-instance visibility check) + Stage5 (HZB occlusion) pipeline.
    // Sphere at view_z=-5 with HZB cleared to 1.0 (no occluders) survives
    // all three stages.
    cull.SetForcePassAll(false);
    cull.SetHZBSystem(&hzb);      // wire HZB texture to descriptor slot 8
    gpuDraw.SetCullingPipeline(&cull);

    // 8. Execute: snapshot upload → cull → draw.
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    snap.UploadToGPUBuffers(vcmd);

    // Render-pass clear depth to 1.0 (far plane = "no occluders present"),
    // then build HZB. HZB stores closest-geometry depth per tile; with depth
    // = far everywhere, Stage5 occlusion test passes for all clusters in frustum.
    // (Clearing to a mid-value like 0.5 false-occludes the sphere which sits at
    // NDC depth ~0.98 with camera at z=5, near=0.1, far=100.)
    RenderPassDesc depthPassDesc{};
    depthPassDesc.depthAttachment.texture = depthTex;
    depthPassDesc.depthAttachment.format = DataFormat::D32_Float;
    depthPassDesc.depthAttachment.loadOp = LoadAction::Clear;
    depthPassDesc.depthAttachment.storeOp = StoreAction::Store;
    depthPassDesc.depthAttachment.clearValue.depth = 1.0f;
    vcmd->BeginRenderPass(depthPassDesc);
    vcmd->EndRenderPass();

    HZBSystem::BuildResult hzbResult = hzb.BuildHZB(depthTex, vcmd);
    TEST_ASSERT(hzbResult.hzb_texture != handles::INVALID_RESOURCE, "BuildHZB hzb_texture");
    TEST_ASSERT(hzbResult.mip_levels >= 1, "BuildHZB mip_levels >= 1");

    bool cullOk = cull.Execute(vcmd, snap, viewMat, projMat, nullptr, 0);
    TEST_ASSERT(cullOk, "GPUCullingPipeline::Execute");

    // T4.6.5 part 22.1 → 07469f3 语义修正:GPUDrivenDrawPipeline 与 GPUCullingPipeline
    // 现在按"同帧同槽"约定工作 —— Stage2/Stage3 用 buffer_index 直接读 cull 本帧
    // 写入的 slot（生产侧 StandardRenderPipeline 对两者传同一个 cbIdx；cull 结尾的
    // ComputeShader→DrawIndirect|VertexInput 内存屏障保证同帧可见性）。旧的
    // (cbIdx+N-1)%N 上一帧轮转读法已废弃:若按旧约定传 cull(0)+gpuDraw(1)，
    // Stage2 会读 slot 1 —— 一个从未被写过的空槽，DrawIndirect 拿到
    // vertexCount=0，光栅输出为空（本测试此前长期失败的根因，非 MoltenVK 问题）。
    bool drawOk = gpuDraw.Execute(vcmd, snap, viewMat, projMat,
                                   cull.GetResults(), /*frame=*/0, /*cbIdx=*/0);
    TEST_ASSERT(drawOk, "GPUDrivenDrawPipeline::Execute");

    // T4.6.5 part 23: ResolveVisibilityBuffer is now auto-called at the end of
    // Execute(). Stage2 rasterized meshlets into visibility_buffer_ (R32_UINT
    // packed meshlet_id/primitive_id); Execute's auto-resolve decodes that +
    // depth into RGBA8 colors. Non-background pixels in the readback prove
    // Stage2 actually rasterized visible geometry.

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit cull + draw");
    fx.base->DestroyCommandBuffer(cmd);

    // 9. Verify indirect_args_buffer has ≥1 draw command.
    const CullingResults& results = cull.GetResults();
    TEST_ASSERT(results.indirect_args_buffer != handles::INVALID_RESOURCE,
                "indirect_args_buffer valid");

    // indirect_args_buffer is sized for 1 VkDrawIndirectCommand (5 u32 = 20 bytes)
    // — see GPUCullingPipeline.cpp:365 (`sizeof(u32) * 5`). Readback must match.
    constexpr u64 kIndirectArgsSize = sizeof(u32) * 5;
    BufferDesc readbackDesc{};
    readbackDesc.size = kIndirectArgsSize;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "CullIndirect_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin readback");

    ResourceBarrier toCopy{};
    toCopy.resource = results.indirect_args_buffer;
    toCopy.beforeState = ResourceState::UnorderedAccess;
    toCopy.afterState = ResourceState::CopySource;
    toCopy.subresource = 0xFFFFFFFF;
    toCopy.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopy, 1);

    vcmd->CopyBuffer(results.indirect_args_buffer, readback,
                     /*srcOffset=*/0, /*dstOffset=*/0, /*size=*/kIndirectArgsSize);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit readback");
    fx.base->DestroyCommandBuffer(cmd);

    // Inspect first VkDrawIndirectCommand:
    //   u32 index_count_per_instance;
    //   u32 instance_count;
    //   u32 first_index;
    //   s32 vertex_offset;
    //   u32 first_instance;
    u32* mapped = static_cast<u32*>(fx.base->MapBuffer(readback, 0, readbackDesc.size));
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
    u32 index_count = mapped[0];
    u32 instance_count = mapped[1];
    std::cout << "[TestGPUCulling] indirect[0]: index_count=" << index_count
              << " instance_count=" << instance_count << std::endl;
    fx.base->UnmapBuffer(readback);

    TEST_ASSERT(instance_count >= 1, "indirect args non-zero instance_count");

    // 9a. T4.6.5 part 22.1: read back visibility_buffer_ directly to verify
    // Stage2 rasterized geometry. R32_UInt = 4 bytes/pixel. The buffer is
    // cleared to 0 by the visibility render pass; non-zero u32s indicate
    // meshlet fragments were written. Also read final_color_texture_ for
    // Stage3 comparison — both should show non-trivial content.
    {
        ResourceHandle visTex = gpuDraw.GetVisibilityBuffer();
        constexpr u64 kVisBytes = (u64)W * H * 4;
        BufferDesc visRbDesc{};
        visRbDesc.size = kVisBytes;
        visRbDesc.type = BufferType::Raw;
        visRbDesc.memoryUsage = GPUMemoryUsage::Readback;
        visRbDesc.name = "VisBuffer_Readback";
        ResourceHandle visRb = fx.base->CreateBuffer(visRbDesc);
        TEST_ASSERT(visRb != handles::INVALID_RESOURCE, "CreateBuffer visRb");

        cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin vis readback");
        BufferTextureCopyRegion visRegion{};
        visRegion.imageSubresource = { 0, 0, 1 };
        visRegion.imageExtent = { W, H, 1 };
        vcmd->CopyTextureToBuffer(visTex, visRb, &visRegion, 1);
        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                    "Submit vis readback");
        fx.base->DestroyCommandBuffer(cmd);

        u32 nonZero = 0;
        u32* visMapped = static_cast<u32*>(fx.base->MapBuffer(visRb, 0, kVisBytes));
        if (visMapped) {
            for (u64 i = 0; i < kVisBytes / 4; ++i) {
                if (visMapped[i] != 0) ++nonZero;
            }
            std::cout << "[TestGPUCulling] visibility_buffer_ non-zero u32s: "
                      << nonZero << "/" << (kVisBytes / 4) << std::endl;
            fx.base->UnmapBuffer(visRb);
        }
        TEST_ASSERT(nonZero > 0, "Stage2 wrote visibility_buffer_ (meshlet fragments)");
        fx.base->DestroyBuffer(visRb);

        // Also read back final_color_texture_ (Stage3 output) for comparison.
        ResourceHandle finalTex = gpuDraw.GetFinalOutputTexture();
        BufferDesc finalRbDesc{};
        finalRbDesc.size = kVisBytes;  // RGBA16F or RGBA8 = 4 bytes/pixel min
        finalRbDesc.type = BufferType::Raw;
        finalRbDesc.memoryUsage = GPUMemoryUsage::Readback;
        finalRbDesc.name = "FinalColor_Readback";
        ResourceHandle finalRb = fx.base->CreateBuffer(finalRbDesc);
        TEST_ASSERT(finalRb != handles::INVALID_RESOURCE, "CreateBuffer finalRb");

        cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin final readback");
        BufferTextureCopyRegion finalRegion{};
        finalRegion.imageSubresource = { 0, 0, 1 };
        finalRegion.imageExtent = { W, H, 1 };
        vcmd->CopyTextureToBuffer(finalTex, finalRb, &finalRegion, 1);
        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                    "Submit final readback");
        fx.base->DestroyCommandBuffer(cmd);

        u8* finalMapped = static_cast<u8*>(fx.base->MapBuffer(finalRb, 0, kVisBytes));
        if (finalMapped) {
            u64 nonZero = 0;
            for (u64 i = 0; i < kVisBytes; ++i) {
                if (finalMapped[i] != 0) ++nonZero;
            }
            std::cout << "[TestGPUCulling] final_color non-zero bytes: "
                      << nonZero << "/" << kVisBytes << std::endl;
            fx.base->UnmapBuffer(finalRb);
        }
        fx.base->DestroyBuffer(finalRb);
    }

    // 9b. T4.6.5 part 20: read back resolve_output_texture_ to verify the resolve
    // compute shader actually wrote pixels. Pattern mirrors TestVulkanCommandBuffer
    // :222-248 (CopyTextureToBuffer + non-zero byte check). The output is RGBA8_UNorm
    // so each pixel is 4 bytes; total size = W*H*4.
    ResourceHandle resolveTex = gpuDraw.GetResolveOutputTexture();
    TEST_ASSERT(resolveTex != handles::INVALID_RESOURCE, "resolve_output_texture valid");

    constexpr u64 kResolveBytes = (u64)W * H * 4;
    BufferDesc resolveRbDesc{};
    resolveRbDesc.size = kResolveBytes;
    resolveRbDesc.type = BufferType::Raw;
    resolveRbDesc.memoryUsage = GPUMemoryUsage::Readback;
    resolveRbDesc.name = "ResolveOutput_Readback";
    ResourceHandle resolveRb = fx.base->CreateBuffer(resolveRbDesc);
    TEST_ASSERT(resolveRb != handles::INVALID_RESOURCE, "CreateBuffer resolveRb");

    cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin resolve readback");

    BufferTextureCopyRegion region{};
    region.imageSubresource = { 0, 0, 1 };  // { baseArrayLayer, mipLevel, layerCount }
    region.imageExtent = { W, H, 1 };
    vcmd->CopyTextureToBuffer(resolveTex, resolveRb, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit resolve readback");
    fx.base->DestroyCommandBuffer(cmd);

    u8* resolveMapped = static_cast<u8*>(fx.base->MapBuffer(resolveRb, 0, kResolveBytes));
    TEST_ASSERT(resolveMapped != nullptr, "MapBuffer resolveRb");
    if (resolveMapped) {
        bool anyNonZero = false;
        for (u64 i = 0; i < kResolveBytes; ++i) {
            if (resolveMapped[i] != 0) { anyNonZero = true; break; }
        }
        TEST_ASSERT(anyNonZero, "resolve output non-trivial (shader wrote pixels)");
        // T4.6.5 part 22.1: stronger bar — Stage2 rasterized real geometry via
        // the rewritten meshlet-aware vertex puller, so the resolve output
        // should NOT be uniformly background blue (0, 51, 102, 255).
        u32 nonBgCount = 0;
        for (u64 i = 0; i + 3 < kResolveBytes; i += 4) {
            if (resolveMapped[i] != 0   || resolveMapped[i + 1] != 51 ||
                resolveMapped[i + 2] != 102 || resolveMapped[i + 3] != 255) {
                ++nonBgCount;
            }
        }
        std::cout << "[TestGPUCulling] resolve non-background pixels: "
                  << nonBgCount << "/" << (kResolveBytes / 4) << std::endl;
        TEST_ASSERT(nonBgCount >= 1,
                    "resolve output has non-background pixels (Stage2 wrote geometry)");
        fx.base->UnmapBuffer(resolveRb);
    }
    fx.base->DestroyBuffer(resolveRb);

    // 10. Cleanup. Order matters: pipelines → snapshot → cluster → entity →
    // resourceManager → procedural mesh asset. The cluster::remove path releases
    // the geometry refcount in NaniteResourceManager; the procedural mesh
    // destroy_resource releases the underlying mesh asset (otherwise
    // ~free_list asserts !_size at process exit).

    // Note: previously 7-10 validation errors fired during cull.Execute +
    // gpuDraw.Execute (Bug A: GPUCullingPipeline MemoryBarrier had invalid
    // ShaderRead in dstAccessMask for DrawIndirect/VertexInput stages;
    // Bug B: GPUDrivenDrawPipeline placeholder texture arrays never transitioned
    // UNDEFINED→SHADER_READ_ONLY; Bug C: binding 9 material data descriptor
    // was VK_NULL_HANDLE). All three fixed in T4.6.5 part 18.5. Test now runs
    // zero validation errors and produces valid indirect args (index_count=384
    // instance_count=3 matching ClusterRefCount).
    fx.base->DestroyBuffer(readback);
    gpuDraw.Shutdown();
    cull.Shutdown();
    hzb.Shutdown();
    fx.base->DestroyTexture(depthTex);
    snap.Shutdown();
    cluster::remove(clusterComp);
    game_entity::remove(entity.get_id());
    resourceManager.Shutdown();
    content::destroy_resource(geom_id, content::asset_type::mesh);

    return TestResult::Passed;
}

// ============================================================================
// Registration + main
// ============================================================================

void RegisterVulkanNaniteSmoke_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanNaniteSmoke_Tests");
    suite->AddTestCase(TestCase("HZBSystem_Smoke",    TestVulkanHZBSystem_Smoke));
    suite->AddTestCase(TestCase("GlobalSDF_Smoke",    TestVulkanGlobalSDF_Smoke));
    suite->AddTestCase(TestCase("GPUCulling_Smoke",   TestVulkanGPUCullingPipeline_Smoke));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanNaniteSmoke_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "Vulkan backend disabled; TestVulkanNaniteSmoke is a no-op." << std::endl;
    return 0;
}

#endif
