#include "Graphics/Utilities/CPUStallAudit.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
// R3（MSVC C1001 预防性拆分）：shader 装载/诊断读回辅助已搬至独立 TU
// （LoadShaderBytecode / DumpRelocatedOriginStats，逐字搬移，行为零变化）。
#include "LumenDDGIPassIO.h"
#include <bit>
#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include "LumenDDGIPass.h"
#include "DDGIAnalyticHistoryPolicy.h"
#include "Engine/Graphics/Lumen/GIDebugParams.h"
#include "Engine/Graphics/Lumen/GIWarmup.h"
#include "Engine/Graphics/Lumen/DDGISlotMap.h"
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Engine/Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#endif
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Dawn/ShaderLoader.h"  // for dawn::LoadWGSL
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/Field/FieldView.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <set>
#include <algorithm>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

#include "DDGIShProducerAudit.inl"

namespace {

struct FinalizeViewData {
    math::m4x4 currentVP;
    math::m4x4 radianceVP;
    math::m4x4 inverseCurrentVP;
    math::m4x4 inverseRadianceVP;
    math::v4 validity; // current depth, radiance depth, world tolerance, unused
};
static_assert(sizeof(FinalizeViewData) == 272);

// ============================================================================
// Descriptor update helper (from TestNaniteStreamingPipeline pattern)
// ============================================================================

struct DescriptorData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

// The Sponza proof currently uploads ~33k nodes / ~262k triangles.  Keep the
// opt-in production bridge bounded so a malformed host request cannot create a
// descriptor-sized allocation or make the fixed 64-entry GPU traversal stack
// meaningless.  These limits are deliberately scene-independent.
static constexpr u64 kFirstSurfaceNodeStride = 48u;
static constexpr u64 kFirstSurfaceTriangleStride = 48u;
static constexpr u64 kFirstSurfaceMaxNodes = 1ull << 20;
static constexpr u64 kFirstSurfaceMaxTriangles = 1ull << 22;
static constexpr u64 kFirstNativeNormalMappingStride = 16u;
static constexpr u64 kFirstNativeNormalAuditVec4Stride = 24u;
static constexpr u64 kFirstNativeNormalAuditCapBytes = 64ull * 1024ull * 1024ull;
static constexpr u64 kNativeVertexStride = 12u;
static constexpr u64 kNativeElementStride = 24u;
static constexpr u64 kNativeInstanceStride = 192u;
static constexpr u64 kNativeMaterialStride = 48u;
static constexpr u64 kAnalyticFirstRhoMappingStride = 32u;
static constexpr u64 kAnalyticFirstRhoAuditVec4Stride = 16u;
static constexpr u64 kAnalyticFirstRhoAuditCapBytes = 64ull * 1024ull * 1024ull;

static void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set,
                                const DescriptorData* params, uint32_t count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (uint32_t i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer ||
            params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage ||
                   params[i].type == DescriptorType::StorageImage ||
                   params[i].type == DescriptorType::CombinedImageSampler ||
                   params[i].type == DescriptorType::Sampler) {
            if (params[i].type == DescriptorType::Sampler) {
                imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            } else {
                imageInfos[i].imageView = params[i].resource;
                imageInfos[i].imageLayout = ResourceState::ShaderResource;
            }
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// ============================================================================
// Shader source loader with #include resolution
// Metal's newLibrary(source) can't resolve #include without include dirs.
// We manually inline local includes before passing to CreateShader.
// ============================================================================

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct LumenDDGIData {
    RGResourceHandle prev_frame_color;
};

} // anonymous namespace

// ============================================================================
// LumenDDGIPass Implementation
// ============================================================================

LumenDDGIPass::LumenDDGIPass() = default;
LumenDDGIPass::~LumenDDGIPass() {
    Shutdown();
}

bool LumenDDGIPass::Initialize(RHIDeviceBase* device, const DDGIRuntimeParams& params) {
    if (initialized_) return true;

    // Phase 2 Vulkan Lumen port: DDGI now active on Vulkan. The two original
    // blockers are resolved — (1) SPIR-V ports exist (DDGITraceRays.spv /
    // DDGIUpdateIrradiance.spv / DDGIUpdateDepth.spv via build_spv.sh naga),
    // (2) CreateDescriptorSetLayouts() has a Vulkan flat-binding branch.
    // See Docs/2026-08-09-vulkan-lumen-gi-port-design.md §Phase 2.

    device_ = device;
    params_ = params;
    if (params_.deferred_field_reset && device_->GetPlatform()!=RHIPlatform::Vulkan) return false;
    if (params_.far_update_budget && (!params_.deferred_field_reset || params_.rays_per_probe!=128 ||
        (params_.far_update_budget!=512 && params_.far_update_budget!=2048))) return false;
    if (params_.far_update_budget) far_update_policy_.Initialize(
        params_.probe_count_x*params_.probe_count_y*params_.probe_count_z,params_.far_update_budget);
    depth_provenance_enabled_=device_->GetPlatform()==RHIPlatform::Vulkan &&
        std::getenv("DDGI_DEPTH_PROVENANCE")!=nullptr;

    // Probe ray sampler mode (M8/T8.4): DEFAULT 1 (per-frame golden-angle
    // jitter) — uniform pdf, so the SH estimator is unchanged; the EMA simply
    // integrates ALL directions over time instead of a fixed set's blind
    // spots (corridor probes missing arch openings forever). 0 = legacy
    // (fixed Fibonacci), 2 = cosine mixture (EXPERIMENTAL — measured 2.4x
    // SC↔probe equilibrium shift, see T8.4). Env override for A/B runs.
    // 2026-09-04 闪烁修复：默认改 mode 0（固定 Fibonacci）。mode 1 的逐帧
    // 黄金角抖动导致 SH 投影逐帧变化 → GI 闪烁（实测帧间差分 mode1=7.0 vs
    // mode0=2.0，3 倍差距）。mode 0 每帧方向固定，temporal blend 后完全稳定。
    // DDGI_RAY_SAMPLE=1 可切回 mode 1（动态场景可能需要）。
    ray_sample_mode_ = 0;
    if (const char* env = std::getenv("DDGI_RAY_SAMPLE")) {
        int m = atoi(env);
        if (((m >= 0 && m <= 5) || m == 7) && ((m != 5 && m != 7) || device_->GetPlatform() == RHIPlatform::Vulkan) && (m != 7 || params_.rays_per_probe == 128)) {
            ray_sample_mode_ = (u32)m;
        } else {
            PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] DDGI_RAY_SAMPLE='" << env << "' invalid (0..4; Vulkan-only 5 or 7 with 128 rays), keeping default" << std::endl;
        }
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] RaySampleMode = " << ray_sample_mode_
              << (ray_sample_mode_ == 0 ? " (legacy uniform Fibonacci)"
                  : ray_sample_mode_ == 1 ? " (per-frame golden-angle jitter)"
                  : ray_sample_mode_ == 2 ? " (cosine mixture around light)"
                  : ray_sample_mode_ == 3 ? " (fixed stratified latitude)"
                  : ray_sample_mode_ == 4 ? " (legacy rays, depth-hole fill on Vulkan)"
                                          : ray_sample_mode_ == 7 ? " (measured oct strata, 128 primary rays)"
                                          : " (legacy radiance, independent oct depth rays)")
              << std::endl;

    const char* measuredGeometryEnv=std::getenv("DDGI_MEASURED_GEOMETRY");
    measured_geometry_requested_=UsesMeasuredVisibility() && measuredGeometryEnv &&
        std::strcmp(measuredGeometryEnv,"1")==0;

    // The first-surface distance candidate is intentionally a Vulkan-only,
    // scalar-mode experiment.  Latching the exact request before descriptor
    // layout creation lets the opt-in pipeline use its independent 16-binding
    // layout while leaving the incumbent 13-binding path untouched.  The
    // geometry itself is supplied later through SetFirstSurfaceGeometry(); a
    // requested-but-unprovisioned candidate never binds null buffers.
    const char* firstSurfaceEnv = std::getenv("DDGI_FIRST_SURFACE");
    first_surface_requested_ =
        device_->GetPlatform() == RHIPlatform::Vulkan &&
        ray_sample_mode_ == 0u && firstSurfaceEnv &&
        std::strcmp(firstSurfaceEnv, "1") == 0;
    if (firstSurfaceEnv && !first_surface_requested_) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_SURFACE] rejected: requires Vulkan and DDGI_RAY_SAMPLE=0"
            << " (requested value='" << firstSurfaceEnv << "')" << std::endl;
    }
    if (first_surface_requested_) {
        const u64 totalProbeCount = u64(params_.probe_count_x) *
                                    u64(params_.probe_count_y) *
                                    u64(params_.probe_count_z);
        if (totalProbeCount == 0 || totalProbeCount > 65535u) {
            first_surface_requested_ = false;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_SURFACE] rejected: probe count " << totalProbeCount
                << " cannot fit the 16-bit audit identity" << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[DDGI_FIRST_SURFACE] requested; waiting for validated BVH resources"
                << std::endl;
        }
    }

    // The zero-cross child is a separate, default-off TraceSDF variant.  It
    // deliberately owns no new geometry contract: SetFirstSurfaceGeometry()
    // supplies the same bounded BVH and audit SSBOs used by the full
    // FIRST_SURFACE experiment.  Fail closed when any full first-surface,
    // native-normal, analytic-rho, or evidence experiment is present so a
    // DUMP2 diagnostic cannot accidentally turn into a mixed candidate run.
    const char* firstZeroCrossEnv = std::getenv("DDGI_FIRST_ZERO_CROSS");
    const bool firstZeroCrossValue = firstZeroCrossEnv &&
                                     std::strcmp(firstZeroCrossEnv, "1") == 0;
    const auto envValueOne = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, "1") == 0;
    };
    const auto envNonEmpty = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] != '\0';
    };
    const bool zeroCrossConflict =
        (firstSurfaceEnv && std::strcmp(firstSurfaceEnv, "1") == 0) ||
        envValueOne("DDGI_FIRST_NATIVE_NORMAL") ||
        envValueOne("DDGI_ANALYTIC_FIRST_RHO") ||
        envNonEmpty("DDGI_ANALYTIC_FIRST_RHO_PREFIX") ||
        envNonEmpty("SDF_GPU_EVIDENCE") ||
        envNonEmpty("DDGI_EVIDENCE_NORMAL") ||
        envNonEmpty("DDGI_DEPTH_ONLY_EVIDENCE") ||
        envNonEmpty("DDGI_SDF_EVIDENCE_MODE");
    first_zero_cross_requested_ =
        firstZeroCrossValue && device_->GetPlatform() == RHIPlatform::Vulkan &&
        ray_sample_mode_ == 0u && !zeroCrossConflict;
    if (firstZeroCrossEnv && !first_zero_cross_requested_) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_ZERO_CROSS] rejected: requires Vulkan, "
               "DDGI_RAY_SAMPLE=0, no full FIRST_SURFACE/FIRST_NATIVE_NORMAL/"
               "analytic-rho/evidence candidate, and value=1 (requested value='"
            << firstZeroCrossEnv << "')" << std::endl;
    }
    if (first_zero_cross_requested_) {
        const u64 totalProbeCount = u64(params_.probe_count_x) *
                                    u64(params_.probe_count_y) *
                                    u64(params_.probe_count_z);
        if (totalProbeCount == 0 || totalProbeCount > 65535u) {
            first_zero_cross_requested_ = false;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_ZERO_CROSS] rejected: probe count " << totalProbeCount
                << " cannot fit the 16-bit audit identity" << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[DDGI_FIRST_ZERO_CROSS] requested; scope=zero-crossing-distance-only, "
                   "default_on=false; waiting for validated BVH resources" << std::endl;
        }
    }

    // The native normal candidate is strictly a child of the first-surface
    // experiment.  Latch the exact request before descriptor-layout creation;
    // a requested-but-unprovisioned source set must fail closed rather than
    // silently selecting the incumbent 13-binding shader.
    const char* firstNativeNormalEnv = std::getenv("DDGI_FIRST_NATIVE_NORMAL");
    first_native_normal_requested_ =
        first_surface_requested_ && firstNativeNormalEnv &&
        std::strcmp(firstNativeNormalEnv, "1") == 0;
    if (firstNativeNormalEnv && !first_native_normal_requested_) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] rejected: requires DDGI_FIRST_SURFACE=1, "
               "Vulkan, and DDGI_RAY_SAMPLE=0 (requested value='"
            << firstNativeNormalEnv << "')" << std::endl;
    } else if (first_native_normal_requested_) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] requested; waiting for live native sources"
            << std::endl;
    }

    // The analytic-first-rho candidate is an independent Finalize variant.
    // The exact scalar-mode gate keeps ray identity and the old probe sampler
    // unchanged.  A diagnostic prefix alone still creates/binds the variant
    // with application disabled, so control and apply runs share the module
    // and descriptor proof.
    const char* analyticFirstRhoEnv = std::getenv("DDGI_ANALYTIC_FIRST_RHO");
    const char* analyticFirstPositionEnv = std::getenv("DDGI_ANALYTIC_FIRST_POSITION");
    const bool analyticPositionControl = analyticFirstPositionEnv &&
        std::strcmp(analyticFirstPositionEnv, "0") == 0;
    const char* analyticFirstRhoPrefix = std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
    const char* ddgiDump = std::getenv("DDGI_DUMP");
    const bool analyticFirstRhoDump2 = ddgiDump && std::strcmp(ddgiDump, "2") == 0;
    analytic_first_rho_apply_ =
        device_->GetPlatform() == RHIPlatform::Vulkan &&
        ray_sample_mode_ == 0u && analyticFirstRhoEnv &&
        std::strcmp(analyticFirstRhoEnv, "1") == 0;
    analytic_first_position_apply_ =
        device_->GetPlatform() == RHIPlatform::Vulkan &&
        ray_sample_mode_ == 0u && analyticFirstPositionEnv &&
        std::strcmp(analyticFirstPositionEnv, "1") == 0 &&
        !analytic_first_rho_apply_;
    analytic_first_rho_prefix_ =
        analyticFirstRhoDump2 && analyticFirstRhoPrefix && analyticFirstRhoPrefix[0] != '\0';
    const char* analyticRhoSourceAudit = std::getenv("DDGI_RAY_SOURCE_AUDIT");
    analytic_first_rho_audit_ = analytic_first_rho_prefix_ && analyticRhoSourceAudit &&
                                std::strcmp(analyticRhoSourceAudit, "1") == 0;
    const char* scFirstAudit = std::getenv("DDGI_SC_FIRST_SOURCE_AUDIT");
    analytic_sc_first_audit_ = analytic_first_rho_audit_ && !analytic_first_rho_apply_ &&
        !analytic_first_position_apply_ && scFirstAudit && std::strcmp(scFirstAudit, "1") == 0;
    if (scFirstAudit && !analytic_sc_first_audit_) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_SC_FIRST_SOURCE_AUDIT] rejected: requires Vulkan scalar mode0, "
               "DDGI_DUMP=2, DDGI_RAY_SOURCE_AUDIT=1, "
               "DDGI_ANALYTIC_FIRST_RHO_PREFIX and no rho/position application"
            << std::endl;
    }
    if (analyticFirstRhoPrefix && analyticFirstRhoPrefix[0] != '\0' &&
        !analyticFirstRhoDump2) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] PREFIX rejected: requires DDGI_DUMP=2"
            << std::endl;
    }
    analytic_first_rho_requested_ =
        device_->GetPlatform() == RHIPlatform::Vulkan &&
        ray_sample_mode_ == 0u &&
        (analytic_first_rho_apply_ || analytic_first_position_apply_ ||
         analyticPositionControl || analytic_first_rho_prefix_);
    if (analyticFirstPositionEnv && !analytic_first_position_apply_ &&
        !analyticPositionControl) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_POSITION] rejected: requires Vulkan, scalar mode0, "
               "value=1 and no simultaneous rho application" << std::endl;
    }
    if (analyticFirstRhoEnv && !analytic_first_rho_apply_) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] application rejected: requires Vulkan, "
               "DDGI_RAY_SAMPLE=0, and value=1 (requested value='"
            << analyticFirstRhoEnv << "')" << std::endl;
    }
    if (analytic_first_rho_requested_) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] requested; application="
            << (analytic_first_rho_apply_ ? 1 : analytic_first_position_apply_ ? 2 : 0)
            << " waiting for validated BVH/native/albedo sources" << std::endl;
    }

    // T4.3: per-mesh detail-SDF hit refinement (first slice of detail
    // tracing). Opt-in: PRIMAL_DDGI_DETAIL_SDF=1 binds up to 4 per-mesh SDF
    // textures near the probe volume; the trace refines coarse GlobalSDF
    // hits and derives gradient normals. Default OFF — hit/normal behavior
    // bit-identical to the pre-T4.3 split trace.
    detail_sdf_enabled_ = (std::getenv("PRIMAL_DDGI_DETAIL_SDF") != nullptr);

    // Probe origin: scene-fit grids place probe[0] on the coverage box's min
    // corner (origin supplied by the pipeline's fit derivation,
    // Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md §5.3); legacy grids
    // center the volume on the world origin. Everything below (position
    // buffer fill, volume_data_.ProbeOrigin sync, GetVolumeAABB) already
    // consumes probe_origin_, so the layout change needs no shader edits.
    if (params_.use_probe_origin) {
        probe_origin_ = params_.probe_origin;
    } else {
        probe_origin_ = {
            -(float)(params_.probe_count_x - 1) * params_.probe_spacing * 0.5f,
            -(float)(params_.probe_count_y - 1) * params_.probe_spacing * 0.5f,
            -(float)(params_.probe_count_z - 1) * params_.probe_spacing * 0.5f,
        };
    }

    // First-class probe position buffer — the DEFINITION of probe placement,
    // written once here before the render loop (static grid). Trace, gather
    // and the debug sphere visualization all consume this same buffer so the
    // notion of "probe position" cannot drift between them. Layout matches
    // ddgiProbeGridCoord: idx = x + y*cx + z*cx*cy.
    {
        const u32 total =
            params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
        BufferDesc pdesc{};
        pdesc.size = (u64)total * sizeof(math::v4);
        pdesc.type = BufferType::Structured;
        pdesc.usage = GPUMemoryUsage::Dynamic;
        pdesc.memoryUsage = GPUMemoryUsage::Dynamic;
        pdesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        probe_positions_ = device_->CreateBuffer(pdesc);
        if (probe_positions_ != handles::INVALID_RESOURCE) {
            if (auto* mapped =
                    static_cast<math::v4*>(device_->MapBuffer(probe_positions_))) {
                for (u32 z = 0; z < params_.probe_count_z; ++z)
                    for (u32 y = 0; y < params_.probe_count_y; ++y)
                        for (u32 x = 0; x < params_.probe_count_x; ++x) {
                            mapped[x + y * params_.probe_count_x +
                                   z * params_.probe_count_x * params_.probe_count_y] =
                                math::v4{probe_origin_.x + (float)x * params_.probe_spacing,
                                         probe_origin_.y + (float)y * params_.probe_spacing,
                                         probe_origin_.z + (float)z * params_.probe_spacing,
                                         0.0f};
                        }
                device_->UnmapBuffer(probe_positions_);
            }
            PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] ProbePositionBuffer filled: " << total
                      << " probes @ spacing " << params_.probe_spacing << std::endl;
        }
    }

    // Sync into volume_data_ up front: UpdateProbeOrigin is disabled for static
    // scenes and the static-bake sync below only runs when a bake exists, so
    // without this the GPU-side origin stays (0,0,0) and the whole probe grid
    // covers the positive octant — scenes centered elsewhere (e.g. Sponza at
    // z<0) fall entirely outside every volume and gather collapses to the dim
    // ambient constant.
    volume_data_.ProbeOrigin = math::v4{probe_origin_.x, probe_origin_.y, probe_origin_.z, 0.0f};

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create compute pipelines + descriptor sets
    CreatePipelines();
    if (depth_provenance_enabled_ && depth_pipeline_==handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_DEPTH_PROVENANCE] shader/pipeline unavailable" << std::endl;
        return false;
    }

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create persistent probe textures
    CreateProbeTextures();
    if (depth_provenance_enabled_ && std::any_of(std::begin(depth_provenance_buffers_),
        std::end(depth_provenance_buffers_),[](ResourceHandle h){return h==handles::INVALID_RESOURCE;})) {
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_DEPTH_PROVENANCE] allocation/zero-init failed" << std::endl;
        for (auto& buffer:depth_provenance_buffers_) {
            if (buffer!=handles::INVALID_RESOURCE) device_->DestroyBuffer(buffer);
            buffer=handles::INVALID_RESOURCE;
        }
        return false;
    }

    // Initialize probes from static bake data (or sky estimate fallback)
    InitializeProbesFromStatic();

    // Initialize probe state tracking for importance-based partial update
    u32 totalProbes = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    probe_states_.resize(totalProbes);
    max_probes_per_frame_ = params_.max_probes_per_frame;

    // Create probe update list buffer (CPU-writable for priority scheduling)
    // Triple-buffered to avoid CPU-GPU race: CPU writes frameIdx's copy while GPU
    // may still be reading the previous frame's copy. Size for worst case (all probes).
    {
        u32 totalProbes = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
        BufferDesc updateListDesc{};
        updateListDesc.size = totalProbes * sizeof(u32);
        // type + memoryUsage are required: leaving type=Unknown makes DawnBuffer
        // fall through to the MapWrite/CopySrc/CopyDst branch with no Storage,
        // so the binding fails validation. The buffer is bound as SSBO in all
        // three DDGI compute shaders.
        updateListDesc.type = BufferType::Structured;
        updateListDesc.usage = GPUMemoryUsage::Dynamic;
        updateListDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        updateListDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        for (u32 i = 0; i < 3; ++i) {
            probe_update_list_buffers_[i] = device_->CreateBuffer(updateListDesc);
        }
    }

    // Initialize probes from static bake data (or sky estimate fallback)
    InitializeProbesFromStatic();

    if (params_.deferred_field_reset) {
        if (!CreateFieldResetTemplate()) return false;
        field_reset_pending_=true;
    }
    initialized_ = true;

    PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Initialized (grid: "
              << params_.probe_count_x << "x" << params_.probe_count_y << "x" << params_.probe_count_z
              << " = " << totalProbes << " probes, "
              << params_.rays_per_probe << " rays/probe)" << std::endl;
    return true;
}

bool LumenDDGIPass::SetFirstSurfaceGeometry(const void* nodes, u64 nodeBytes,
                                            const void* triangles, u64 triangleBytes) {
    const bool zeroCross = first_zero_cross_requested_;
    const char* candidateName = zeroCross ? "DDGI_FIRST_ZERO_CROSS" : "DDGI_FIRST_SURFACE";
    auto reject = [&](const char* reason) {
        first_surface_enabled_ = false;
        first_zero_cross_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[" << candidateName << "] rejected: " << reason << std::endl;
        return false;
    };

    if ((!first_surface_requested_ && !first_zero_cross_requested_) || !device_ ||
        device_->GetPlatform() != RHIPlatform::Vulkan || ray_sample_mode_ != 0u)
        return reject("request is not active (requires Vulkan and scalar mode 0)");
    if (!nodes || !triangles || nodeBytes == 0 || triangleBytes == 0)
        return reject("null or empty geometry");
    if ((nodeBytes % kFirstSurfaceNodeStride) != 0 ||
        (triangleBytes % kFirstSurfaceTriangleStride) != 0)
        return reject("geometry byte size is not a 48-byte record multiple");

    const u64 nodeCount = nodeBytes / kFirstSurfaceNodeStride;
    const u64 triangleCount = triangleBytes / kFirstSurfaceTriangleStride;
    if (nodeCount == 0 || nodeCount > kFirstSurfaceMaxNodes ||
        triangleCount == 0 || triangleCount > kFirstSurfaceMaxTriangles)
        return reject("geometry exceeds bounded node/triangle capacity");
    const u64 maxU64 = std::numeric_limits<u64>::max();
    const u64 probeXY = u64(params_.probe_count_x) * u64(params_.probe_count_y);
    if (params_.probe_count_z != 0 && probeXY > maxU64 / u64(params_.probe_count_z))
        return reject("probe count multiplication overflow");
    const u64 totalProbes = probeXY * u64(params_.probe_count_z);
    if (params_.rays_per_probe != 0 && totalProbes > maxU64 / u64(params_.rays_per_probe))
        return reject("ray count multiplication overflow");
    const u64 totalRays = totalProbes * u64(params_.rays_per_probe);
    if (totalRays > maxU64 / 3u)
        return reject("audit record count multiplication overflow");
    const u64 auditVec4Count = totalRays * 3u;
    if (totalProbes == 0 || totalProbes > 65535u || totalRays == 0 ||
        totalRays > (maxU64 / (3u * sizeof(math::v4))) ||
        auditVec4Count > std::numeric_limits<u32>::max())
        return reject("probe/ray capacity is invalid for the audit identity");
    const auto candidatePipeline = zeroCross ? first_zero_cross_sdf_trace_pipeline_
                                             : first_surface_sdf_trace_pipeline_;
    if (candidatePipeline == handles::INVALID_PIPELINE ||
        first_surface_sdf_trace_layout_ == handles::INVALID_PIPELINE_LAYOUT ||
        first_surface_sdf_trace_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return reject(zeroCross ? "zero-cross SPIR-V pipeline/layout is unavailable"
                                : "first-surface SPIR-V pipeline/layout is unavailable");
    for (const auto set : first_surface_sdf_trace_ds_) {
        if (set == handles::INVALID_DESCRIPTOR_SET)
            return reject("first-surface descriptor set allocation failed");
    }

    BufferDesc nodeDesc{};
    nodeDesc.size = nodeBytes;
    nodeDesc.type = BufferType::Structured;
    nodeDesc.usage = GPUMemoryUsage::Static;
    nodeDesc.memoryUsage = GPUMemoryUsage::Static;
    nodeDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    nodeDesc.structured.elementCount = static_cast<u32>(nodeCount * 3u);
    nodeDesc.structured.elementStride = sizeof(math::v4);
    nodeDesc.name = "DDGI_FirstSurfaceNodes";

    BufferDesc triangleDesc{};
    triangleDesc.size = triangleBytes;
    triangleDesc.type = BufferType::Structured;
    triangleDesc.usage = GPUMemoryUsage::Static;
    triangleDesc.memoryUsage = GPUMemoryUsage::Static;
    triangleDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    triangleDesc.structured.elementCount = static_cast<u32>(triangleCount * 3u);
    triangleDesc.structured.elementStride = sizeof(math::v4);
    triangleDesc.name = "DDGI_FirstSurfaceTriangles";

    BufferDesc auditDesc{};
    auditDesc.size = auditVec4Count * sizeof(math::v4);
    auditDesc.type = BufferType::Structured;
    auditDesc.usage = GPUMemoryUsage::Dynamic;
    auditDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    auditDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    auditDesc.structured.elementCount = static_cast<u32>(auditVec4Count);
    auditDesc.structured.elementStride = sizeof(math::v4);
    auditDesc.name = "DDGI_FirstSurfaceAudit";

    ResourceHandle newNodes = device_->CreateBuffer(nodeDesc);
    ResourceHandle newTriangles = device_->CreateBuffer(triangleDesc);
    ResourceHandle newAudit = device_->CreateBuffer(auditDesc);
    auto destroyNew = [&]() {
        if (newNodes != handles::INVALID_RESOURCE) device_->DestroyBuffer(newNodes);
        if (newTriangles != handles::INVALID_RESOURCE) device_->DestroyBuffer(newTriangles);
        if (newAudit != handles::INVALID_RESOURCE) device_->DestroyBuffer(newAudit);
    };
    if (newNodes == handles::INVALID_RESOURCE ||
        newTriangles == handles::INVALID_RESOURCE ||
        newAudit == handles::INVALID_RESOURCE) {
        destroyNew();
        return reject("geometry/audit buffer allocation failed");
    }
    if (!device_->UpdateBufferData(newNodes, nodes, nodeBytes, 0) ||
        !device_->UpdateBufferData(newTriangles, triangles, triangleBytes, 0)) {
        destroyNew();
        return reject("geometry upload failed");
    }
    if (void* mapped = device_->MapBuffer(newAudit)) {
        std::memset(mapped, 0, static_cast<size_t>(auditDesc.size));
        device_->UnmapBuffer(newAudit);
    } else {
        destroyNew();
        return reject("audit buffer map/initialization failed");
    }

    // Replace only after every new resource is complete.  The call is made
    // before the first render, so the old resources cannot be in flight here.
    if (first_surface_nodes_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(first_surface_nodes_buffer_);
    if (first_surface_triangles_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(first_surface_triangles_buffer_);
    if (first_surface_audit_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(first_surface_audit_buffer_);
    first_surface_nodes_buffer_ = newNodes;
    first_surface_triangles_buffer_ = newTriangles;
    first_surface_audit_buffer_ = newAudit;
    first_surface_enabled_ = true;
    first_zero_cross_enabled_ = zeroCross;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[" << candidateName << "] enabled: nodes=" << nodeCount
        << " triangles=" << triangleCount << " audit_vec4=" << auditVec4Count
        << " (3 records/ray, scalar mode 0, scope="
        << (zeroCross ? "zero-crossing-distance-only" : "first-surface-distance")
        << ", default_on=false)" << std::endl;
    return true;
}

bool LumenDDGIPass::SetFirstNativeNormalSources(
    ResourceHandle positions, ResourceHandle elements, ResourceHandle instances,
    ResourceHandle materials, ResourceHandle normalTexture,
    SamplerHandle normalSampler, const void* triangleMapping,
    u64 triangleMappingBytes) {
    auto reject = [&](const char* reason) {
        first_native_normal_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] rejected: " << reason << std::endl;
        return false;
    };
    if (!first_native_normal_requested_ || !first_surface_enabled_ || !device_ ||
        device_->GetPlatform() != RHIPlatform::Vulkan || ray_sample_mode_ != 0u)
        return reject("request is not active (requires first-surface Vulkan scalar mode 0)");
    if (first_native_normal_sdf_trace_pipeline_ == handles::INVALID_PIPELINE ||
        first_native_normal_sdf_trace_layout_ == handles::INVALID_PIPELINE_LAYOUT ||
        first_native_normal_sdf_trace_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return reject("first-native SPIR-V pipeline/layout is unavailable");
    for (const auto set : first_native_normal_sdf_trace_ds_) {
        if (set == handles::INVALID_DESCRIPTOR_SET)
            return reject("first-native descriptor set allocation failed");
    }
    if (positions == handles::INVALID_RESOURCE ||
        elements == handles::INVALID_RESOURCE ||
        instances == handles::INVALID_RESOURCE ||
        materials == handles::INVALID_RESOURCE ||
        normalTexture == handles::INVALID_RESOURCE ||
        normalSampler == handles::INVALID_SAMPLER || !triangleMapping ||
        triangleMappingBytes == 0u ||
        (triangleMappingBytes % kFirstNativeNormalMappingStride) != 0u)
        return reject("native source handle, sampler, or mapping is missing");

#if !defined(ENABLE_VULKAN) || !ENABLE_VULKAN
    // VulkanDevice/VulkanTexture are backend-only types.  Keep this public
    // opt-in entry point linkable in Metal/Dawn builds while failing closed;
    // the incumbent first-surface/default path remains untouched there.
    return reject("Vulkan backend is not compiled");
#else
    auto* vk = static_cast<VulkanDevice*>(device_);
    u64 positionBytes = 0, elementBytes = 0, instanceBytes = 0, materialBytes = 0;
    if (!vk->GetBufferByteSize(positions, positionBytes) ||
        !vk->GetBufferByteSize(elements, elementBytes) ||
        !vk->GetBufferByteSize(instances, instanceBytes) ||
        !vk->GetBufferByteSize(materials, materialBytes) ||
        positionBytes == 0 || elementBytes == 0 || instanceBytes == 0 ||
        materialBytes == 0)
        return reject("native source buffer size query failed");
    if ((positionBytes % kNativeVertexStride) != 0u ||
        (elementBytes % kNativeElementStride) != 0u ||
        (instanceBytes % kNativeInstanceStride) != 0u ||
        (materialBytes % kNativeMaterialStride) != 0u ||
        elementBytes != (positionBytes / kNativeVertexStride) * kNativeElementStride)
        return reject("native source strides do not match GPUDrivenDraw");
    const u64 vertexCount = positionBytes / kNativeVertexStride;
    const u64 elementCount = elementBytes / kNativeElementStride;
    const u64 instanceCount = instanceBytes / kNativeInstanceStride;
    const u64 materialCount = materialBytes / kNativeMaterialStride;
    if (vertexCount == 0 || vertexCount > 16ull * 1024ull * 1024ull ||
        elementCount == 0 || instanceCount == 0 || instanceCount > 1ull * 1024ull * 1024ull ||
        materialCount == 0 || materialCount > 1ull * 1024ull * 1024ull)
        return reject("native source count is outside the bounded contract");

    auto* normalTex = vk->GetTexture(normalTexture);
    if (!normalTex) return reject("native normal texture lookup failed");
    const TextureDesc& normalDesc = normalTex->GetTextureDesc();
    if (normalDesc.type != TextureType::Texture2DArray ||
        normalDesc.size.x == 0 || normalDesc.size.y == 0 ||
        normalDesc.arraySize == 0 || normalDesc.mipLevels == 0 ||
        (normalDesc.format != DataFormat::RGBA8_UNorm &&
         normalDesc.format != DataFormat::RGBA8_sRGB))
        return reject("native normal texture must be a nonempty RGBA8 2D array");

    u64 triangleBytes = 0;
    if (first_surface_triangles_buffer_ == handles::INVALID_RESOURCE ||
        !vk->GetBufferByteSize(first_surface_triangles_buffer_, triangleBytes) ||
        triangleBytes == 0 || (triangleBytes % kFirstSurfaceTriangleStride) != 0u ||
        triangleMappingBytes / kFirstNativeNormalMappingStride !=
            triangleBytes / kFirstSurfaceTriangleStride)
        return reject("triangle mapping does not match the uploaded first-surface table");

    // The trace shader indexes this audit by update-slot ray.  Keep a fixed,
    // descriptor-visible bound so an unexpectedly large update budget fails
    // closed at the host reader instead of creating an unbounded allocation.
    const u64 maxAuditVec4 = kFirstNativeNormalAuditCapBytes / sizeof(math::v4);
    const u64 auditRayCapacity = maxAuditVec4 / kFirstNativeNormalAuditVec4Stride;
    if (auditRayCapacity == 0 ||
        auditRayCapacity > std::numeric_limits<u32>::max() /
                               kFirstNativeNormalAuditVec4Stride)
        return reject("native audit capacity arithmetic overflow");
    const u64 auditVec4Count = auditRayCapacity * kFirstNativeNormalAuditVec4Stride;
    const u64 auditBytes = auditVec4Count * sizeof(math::v4);

    BufferDesc mappingDesc{};
    mappingDesc.size = triangleMappingBytes;
    mappingDesc.type = BufferType::Structured;
    mappingDesc.usage = GPUMemoryUsage::Static;
    mappingDesc.memoryUsage = GPUMemoryUsage::Static;
    mappingDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    mappingDesc.structured.elementCount = static_cast<u32>(triangleMappingBytes /
                                                             kFirstNativeNormalMappingStride);
    mappingDesc.structured.elementStride = kFirstNativeNormalMappingStride;
    mappingDesc.name = "DDGI_FirstNativeNormalTriangleMapping";

    BufferDesc auditDesc{};
    auditDesc.size = auditBytes;
    auditDesc.type = BufferType::Structured;
    auditDesc.usage = GPUMemoryUsage::Dynamic;
    auditDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    auditDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    auditDesc.structured.elementCount = static_cast<u32>(auditVec4Count);
    auditDesc.structured.elementStride = sizeof(math::v4);
    auditDesc.name = "DDGI_FirstNativeNormalAudit";

    ResourceHandle newMapping = device_->CreateBuffer(mappingDesc);
    ResourceHandle newAudit = device_->CreateBuffer(auditDesc);
    auto destroyNew = [&]() {
        if (newMapping != handles::INVALID_RESOURCE) device_->DestroyBuffer(newMapping);
        if (newAudit != handles::INVALID_RESOURCE) device_->DestroyBuffer(newAudit);
    };
    if (newMapping == handles::INVALID_RESOURCE || newAudit == handles::INVALID_RESOURCE) {
        destroyNew();
        return reject("native mapping/audit buffer allocation failed");
    }
    if (!device_->UpdateBufferData(newMapping, triangleMapping, triangleMappingBytes, 0)) {
        destroyNew();
        return reject("native triangle mapping upload failed");
    }
    if (void* mapped = device_->MapBuffer(newAudit)) {
        std::memset(mapped, 0, static_cast<size_t>(auditBytes));
        device_->UnmapBuffer(newAudit);
    } else {
        destroyNew();
        return reject("native audit buffer initialization failed");
    }

    // Source handles belong to GPUDrivenDraw/material setup.  Replace only
    // after the owned buffers are complete; the setter is called before the
    // first candidate dispatch or during a deferred pass rebuild.
    if (first_native_normal_triangle_mapping_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(first_native_normal_triangle_mapping_buffer_);
    if (first_native_normal_audit_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(first_native_normal_audit_buffer_);
    first_native_normal_positions_ = positions;
    first_native_normal_elements_ = elements;
    first_native_normal_instances_ = instances;
    first_native_normal_materials_ = materials;
    first_native_normal_texture_ = normalTexture;
    first_native_normal_sampler_ = normalSampler;
    first_native_normal_triangle_mapping_buffer_ = newMapping;
    first_native_normal_audit_buffer_ = newAudit;
    first_native_normal_audit_bytes_ = auditBytes;
    first_native_normal_enabled_ = true;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_NATIVE_NORMAL] enabled: vertices=" << vertexCount
        << " elements=" << elementCount << " instances=" << instanceCount
        << " materials=" << materialCount << " mapping_records="
        << (triangleMappingBytes / kFirstNativeNormalMappingStride)
        << " audit_bytes=" << auditBytes << " audit_ray_capacity=" << auditRayCapacity
        << " format=" << static_cast<u32>(normalDesc.format) << std::endl;
    return true;
#endif
}

bool LumenDDGIPass::SetAnalyticFirstRhoSources(
    const void* nodes, u64 nodeBytes, const void* triangles, u64 triangleBytes,
    ResourceHandle positions, ResourceHandle elements, ResourceHandle instances,
    ResourceHandle materials, ResourceHandle albedoTexture,
    SamplerHandle albedoSampler, const void* triangleMapping,
    u64 triangleMappingBytes) {
    auto reject = [&](const char* reason) {
        analytic_first_rho_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] rejected: " << reason << std::endl;
        return false;
    };
    if (!analytic_first_rho_requested_ || !device_ ||
        device_->GetPlatform() != RHIPlatform::Vulkan || ray_sample_mode_ != 0u)
        return reject("request is not active (requires Vulkan scalar mode 0)");
    if (!nodes || !triangles || nodeBytes == 0 || triangleBytes == 0 ||
        !triangleMapping || triangleMappingBytes == 0 ||
        positions == handles::INVALID_RESOURCE ||
        elements == handles::INVALID_RESOURCE ||
        instances == handles::INVALID_RESOURCE ||
        materials == handles::INVALID_RESOURCE ||
        albedoTexture == handles::INVALID_RESOURCE ||
        albedoSampler == handles::INVALID_SAMPLER)
        return reject("candidate source handle, sampler, or geometry is missing");
    if ((nodeBytes % kFirstSurfaceNodeStride) != 0 ||
        (triangleBytes % kFirstSurfaceTriangleStride) != 0 ||
        (triangleMappingBytes % kAnalyticFirstRhoMappingStride) != 0)
        return reject("geometry/mapping does not use the bounded record strides");
    const u64 nodeCount = nodeBytes / kFirstSurfaceNodeStride;
    const u64 triangleCount = triangleBytes / kFirstSurfaceTriangleStride;
    const u64 mappingCount = triangleMappingBytes / kAnalyticFirstRhoMappingStride;
    if (nodeCount == 0 || nodeCount > kFirstSurfaceMaxNodes ||
        triangleCount == 0 || triangleCount > kFirstSurfaceMaxTriangles ||
        mappingCount != triangleCount)
        return reject("geometry or mapping count is outside the bounded contract");
    const u64 probeXY = u64(params_.probe_count_x) * u64(params_.probe_count_y);
    if (params_.probe_count_z != 0 &&
        probeXY > std::numeric_limits<u64>::max() / u64(params_.probe_count_z))
        return reject("probe count multiplication overflow");
    const u64 totalProbes = probeXY * u64(params_.probe_count_z);
    if (params_.rays_per_probe == 0 ||
        totalProbes > std::numeric_limits<u64>::max() / u64(params_.rays_per_probe))
        return reject("ray count multiplication overflow");
    const u64 totalRays = totalProbes * u64(params_.rays_per_probe);
    const u64 maxAuditVec4 = kAnalyticFirstRhoAuditCapBytes / sizeof(math::v4);
    const u64 maxAuditRays = maxAuditVec4 / kAnalyticFirstRhoAuditVec4Stride;
    if (totalProbes == 0 || totalRays == 0 || totalRays > maxAuditRays ||
        totalRays > std::numeric_limits<u64>::max() /
                         (kAnalyticFirstRhoAuditVec4Stride * sizeof(math::v4)))
        return reject("full update-ray capacity exceeds the fixed audit bound");
    if (analytic_first_rho_finalize_pipeline_ == handles::INVALID_PIPELINE ||
        analytic_first_rho_finalize_layout_ == handles::INVALID_PIPELINE_LAYOUT ||
        analytic_first_rho_finalize_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        return reject("analytic-first-rho SPIR-V pipeline/layout is unavailable");
    for (const auto set : analytic_first_rho_finalize_ds_) {
        if (set == handles::INVALID_DESCRIPTOR_SET)
            return reject("analytic-first-rho descriptor set allocation failed");
    }

#if !defined(ENABLE_VULKAN) || !ENABLE_VULKAN
    return reject("Vulkan backend is not compiled");
#else
    auto* vk = static_cast<VulkanDevice*>(device_);
    u64 positionBytes = 0, elementBytes = 0, instanceBytes = 0, materialBytes = 0;
    if (!vk->GetBufferByteSize(positions, positionBytes) ||
        !vk->GetBufferByteSize(elements, elementBytes) ||
        !vk->GetBufferByteSize(instances, instanceBytes) ||
        !vk->GetBufferByteSize(materials, materialBytes) ||
        positionBytes == 0 || elementBytes == 0 || instanceBytes == 0 || materialBytes == 0)
        return reject("native source buffer size query failed");
    if (positionBytes % kNativeVertexStride != 0 ||
        elementBytes % kNativeElementStride != 0 ||
        instanceBytes % kNativeInstanceStride != 0 ||
        materialBytes % kNativeMaterialStride != 0 ||
        elementBytes != (positionBytes / kNativeVertexStride) * kNativeElementStride)
        return reject("native source strides do not match GPUDrivenDraw");
    const u64 vertexCount = positionBytes / kNativeVertexStride;
    const u64 elementCount = elementBytes / kNativeElementStride;
    const u64 instanceCount = instanceBytes / kNativeInstanceStride;
    const u64 materialCount = materialBytes / kNativeMaterialStride;
    if (vertexCount == 0 || vertexCount > 16ull * 1024ull * 1024ull ||
        elementCount == 0 || instanceCount == 0 || instanceCount > 1ull * 1024ull * 1024ull ||
        materialCount == 0 || materialCount > 1ull * 1024ull * 1024ull)
        return reject("native source count is outside the bounded contract");
    auto* albedo = vk->GetTexture(albedoTexture);
    if (!albedo) return reject("albedo texture lookup failed");
    const TextureDesc& albedoDesc = albedo->GetTextureDesc();
    if (albedoDesc.type != TextureType::Texture2DArray || albedoDesc.size.x == 0 ||
        albedoDesc.size.y == 0 || albedoDesc.arraySize == 0 || albedoDesc.mipLevels == 0 ||
        (albedoDesc.format != DataFormat::RGBA8_UNorm &&
         albedoDesc.format != DataFormat::RGBA8_sRGB))
        return reject("albedo texture must be a nonempty RGBA8 2D array");

    const u64 auditBytes = totalRays * kAnalyticFirstRhoAuditVec4Stride * sizeof(math::v4);
    BufferDesc nodeDesc{};
    nodeDesc.size = nodeBytes;
    nodeDesc.type = BufferType::Structured;
    nodeDesc.usage = GPUMemoryUsage::Static;
    nodeDesc.memoryUsage = GPUMemoryUsage::Static;
    nodeDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    nodeDesc.structured.elementCount = static_cast<u32>(nodeCount * 3u);
    nodeDesc.structured.elementStride = sizeof(math::v4);
    nodeDesc.name = "DDGI_AnalyticFirstRhoNodes";
    BufferDesc triangleDesc = nodeDesc;
    triangleDesc.size = triangleBytes;
    triangleDesc.structured.elementCount = static_cast<u32>(triangleCount * 3u);
    triangleDesc.name = "DDGI_AnalyticFirstRhoTriangles";
    BufferDesc mappingDesc{};
    mappingDesc.size = triangleMappingBytes;
    mappingDesc.type = BufferType::Structured;
    mappingDesc.usage = GPUMemoryUsage::Static;
    mappingDesc.memoryUsage = GPUMemoryUsage::Static;
    mappingDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    mappingDesc.structured.elementCount = static_cast<u32>(mappingCount * 2u);
    mappingDesc.structured.elementStride = sizeof(math::v4);
    mappingDesc.name = "DDGI_AnalyticFirstRhoMapping";
    BufferDesc auditDesc{};
    auditDesc.size = auditBytes;
    auditDesc.type = BufferType::Structured;
    auditDesc.usage = GPUMemoryUsage::Dynamic;
    auditDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    auditDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    auditDesc.structured.elementCount = static_cast<u32>(totalRays * kAnalyticFirstRhoAuditVec4Stride);
    auditDesc.structured.elementStride = sizeof(math::v4);
    auditDesc.name = "DDGI_AnalyticFirstRhoAudit";
    ResourceHandle newNodes = device_->CreateBuffer(nodeDesc);
    ResourceHandle newTriangles = device_->CreateBuffer(triangleDesc);
    ResourceHandle newMapping = device_->CreateBuffer(mappingDesc);
    ResourceHandle newAudit = device_->CreateBuffer(auditDesc);
    auto destroyNew = [&]() {
        if (newNodes != handles::INVALID_RESOURCE) device_->DestroyBuffer(newNodes);
        if (newTriangles != handles::INVALID_RESOURCE) device_->DestroyBuffer(newTriangles);
        if (newMapping != handles::INVALID_RESOURCE) device_->DestroyBuffer(newMapping);
        if (newAudit != handles::INVALID_RESOURCE) device_->DestroyBuffer(newAudit);
    };
    if (newNodes == handles::INVALID_RESOURCE || newTriangles == handles::INVALID_RESOURCE ||
        newMapping == handles::INVALID_RESOURCE || newAudit == handles::INVALID_RESOURCE) {
        destroyNew();
        return reject("candidate BVH/mapping/audit allocation failed");
    }
    if (!device_->UpdateBufferData(newNodes, nodes, nodeBytes, 0) ||
        !device_->UpdateBufferData(newTriangles, triangles, triangleBytes, 0) ||
        !device_->UpdateBufferData(newMapping, triangleMapping, triangleMappingBytes, 0)) {
        destroyNew();
        return reject("candidate BVH/mapping upload failed");
    }
    if (void* mapped = device_->MapBuffer(newAudit)) {
        std::memset(mapped, 0, static_cast<size_t>(auditBytes));
        device_->UnmapBuffer(newAudit);
    } else {
        destroyNew();
        return reject("candidate audit buffer map/initialization failed");
    }
    if (analytic_first_rho_nodes_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(analytic_first_rho_nodes_buffer_);
    if (analytic_first_rho_triangles_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(analytic_first_rho_triangles_buffer_);
    if (analytic_first_rho_mapping_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(analytic_first_rho_mapping_buffer_);
    if (analytic_first_rho_audit_buffer_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(analytic_first_rho_audit_buffer_);
    analytic_first_rho_nodes_buffer_ = newNodes;
    analytic_first_rho_triangles_buffer_ = newTriangles;
    analytic_first_rho_mapping_buffer_ = newMapping;
    analytic_first_rho_audit_buffer_ = newAudit;
    analytic_first_rho_audit_bytes_ = auditBytes;
    analytic_first_rho_positions_ = positions;
    analytic_first_rho_elements_ = elements;
    analytic_first_rho_instances_ = instances;
    analytic_first_rho_materials_ = materials;
    analytic_first_rho_albedo_texture_ = albedoTexture;
    analytic_first_rho_albedo_sampler_ = albedoSampler;
    analytic_first_rho_enabled_ = true;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_ANALYTIC_FIRST_RHO] enabled: nodes=" << nodeCount
        << " triangles=" << triangleCount << " native_vertices=" << vertexCount
        << " instances=" << instanceCount << " materials=" << materialCount
        << " mapping_records=" << mappingCount << " audit_bytes=" << auditBytes
        << " application=" << (analytic_first_rho_apply_ ? 1 : 0) << std::endl;
    return true;
#endif
}

bool LumenDDGIPass::FlushSHProducerAudit(u32 absoluteFrame) {
    if (!DDGIShProducerAudit::Requested()) return true;
    const char* f = std::getenv("DDGI_SH_PRODUCER_AUDIT_FRAME");
    if (f && absoluteFrame < static_cast<u32>(std::strtoul(f,nullptr,10))) return true;
    return sh_producer_audit_ && sh_producer_audit_->Flush(absoluteFrame);
}

void LumenDDGIPass::Shutdown() {
    // A rejected opt-in can leave the device/pipeline members alive while
    // Initialize() is unwinding before initialized_ is set.  Keep the
    // candidate cleanup reachable for that partial-lifecycle case too.
    if (!initialized_ && !device_) return;
    initialized_ = false;
    sh_producer_audit_.reset();
    if(field_reset_template_!=handles::INVALID_RESOURCE) device_->DestroyBuffer(field_reset_template_);
    field_reset_template_=handles::INVALID_RESOURCE;
    field_reset_pending_=false;

    variance_fence_ticket_.Reset();variance_cpu_confirmed_=false;variance_identity_valid_=false;variance_scene_epoch_=0;variance_source_digest_=0;variance_identity_.Reset();variance_scene_provider_=nullptr;
    depth_readback_ring_.shutdown(device_);
    for (auto& buffer:depth_provenance_buffers_) {
        if (buffer!=handles::INVALID_RESOURCE) device_->DestroyBuffer(buffer);
        buffer=handles::INVALID_RESOURCE;
    }
    depth_provenance_enabled_=false;
    analytic_audit_enabled_=false;
    ray_source_audit_enabled_=false;
    query_source_audit_enabled_=false;

    // First-surface resources are allocated after Initialize() by an explicit
    // host call, so release them here rather than relying on the incumbent
    // pass's deferred-handle convention.  No in-flight render may remain when
    // Shutdown is entered by the owner.
    if (device_) {
        for (auto& set : first_surface_sdf_trace_ds_) {
            if (set != handles::INVALID_DESCRIPTOR_SET) {
                device_->DestroyDescriptorSet(set);
                set = handles::INVALID_DESCRIPTOR_SET;
            }
        }
        for (auto& set : first_native_normal_sdf_trace_ds_) {
            if (set != handles::INVALID_DESCRIPTOR_SET) {
                device_->DestroyDescriptorSet(set);
                set = handles::INVALID_DESCRIPTOR_SET;
            }
        }
        if (first_surface_sdf_trace_pipeline_ != handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(first_surface_sdf_trace_pipeline_);
            first_surface_sdf_trace_pipeline_ = handles::INVALID_PIPELINE;
        }
        if (first_zero_cross_sdf_trace_pipeline_ != handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(first_zero_cross_sdf_trace_pipeline_);
            first_zero_cross_sdf_trace_pipeline_ = handles::INVALID_PIPELINE;
        }
        if (first_native_normal_sdf_trace_pipeline_ != handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(first_native_normal_sdf_trace_pipeline_);
            first_native_normal_sdf_trace_pipeline_ = handles::INVALID_PIPELINE;
        }
        if (first_surface_sdf_trace_shader_ != handles::INVALID_SHADER) {
            device_->DestroyShader(first_surface_sdf_trace_shader_);
            first_surface_sdf_trace_shader_ = handles::INVALID_SHADER;
        }
        if (first_zero_cross_sdf_trace_shader_ != handles::INVALID_SHADER) {
            device_->DestroyShader(first_zero_cross_sdf_trace_shader_);
            first_zero_cross_sdf_trace_shader_ = handles::INVALID_SHADER;
        }
        if (first_native_normal_sdf_trace_shader_ != handles::INVALID_SHADER) {
            device_->DestroyShader(first_native_normal_sdf_trace_shader_);
            first_native_normal_sdf_trace_shader_ = handles::INVALID_SHADER;
        }
        if (first_surface_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            device_->DestroyPipelineLayout(first_surface_sdf_trace_layout_);
            first_surface_sdf_trace_layout_ = handles::INVALID_PIPELINE_LAYOUT;
        }
        if (first_native_normal_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            device_->DestroyPipelineLayout(first_native_normal_sdf_trace_layout_);
            first_native_normal_sdf_trace_layout_ = handles::INVALID_PIPELINE_LAYOUT;
        }
        if (first_surface_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_->DestroyDescriptorSetLayout(first_surface_sdf_trace_set_layout_);
            first_surface_sdf_trace_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        }
        if (first_native_normal_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_->DestroyDescriptorSetLayout(first_native_normal_sdf_trace_set_layout_);
            first_native_normal_sdf_trace_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        }
        if (first_surface_nodes_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(first_surface_nodes_buffer_);
            first_surface_nodes_buffer_ = handles::INVALID_RESOURCE;
        }
        if (first_surface_triangles_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(first_surface_triangles_buffer_);
            first_surface_triangles_buffer_ = handles::INVALID_RESOURCE;
        }
        if (first_surface_audit_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(first_surface_audit_buffer_);
            first_surface_audit_buffer_ = handles::INVALID_RESOURCE;
        }
        if (first_native_normal_triangle_mapping_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(first_native_normal_triangle_mapping_buffer_);
            first_native_normal_triangle_mapping_buffer_ = handles::INVALID_RESOURCE;
        }
        if (first_native_normal_audit_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(first_native_normal_audit_buffer_);
            first_native_normal_audit_buffer_ = handles::INVALID_RESOURCE;
        }
        for (auto& set : analytic_first_rho_finalize_ds_) {
            if (set != handles::INVALID_DESCRIPTOR_SET) {
                device_->DestroyDescriptorSet(set);
                set = handles::INVALID_DESCRIPTOR_SET;
            }
        }
        if (analytic_first_rho_finalize_pipeline_ != handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(analytic_first_rho_finalize_pipeline_);
            analytic_first_rho_finalize_pipeline_ = handles::INVALID_PIPELINE;
        }
        if (analytic_first_rho_finalize_shader_ != handles::INVALID_SHADER) {
            device_->DestroyShader(analytic_first_rho_finalize_shader_);
            analytic_first_rho_finalize_shader_ = handles::INVALID_SHADER;
        }
        if (analytic_first_rho_finalize_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            device_->DestroyPipelineLayout(analytic_first_rho_finalize_layout_);
            analytic_first_rho_finalize_layout_ = handles::INVALID_PIPELINE_LAYOUT;
        }
        if (analytic_first_rho_finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_->DestroyDescriptorSetLayout(analytic_first_rho_finalize_set_layout_);
            analytic_first_rho_finalize_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        }
        if (analytic_first_rho_nodes_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(analytic_first_rho_nodes_buffer_);
            analytic_first_rho_nodes_buffer_ = handles::INVALID_RESOURCE;
        }
        if (analytic_first_rho_triangles_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(analytic_first_rho_triangles_buffer_);
            analytic_first_rho_triangles_buffer_ = handles::INVALID_RESOURCE;
        }
        if (analytic_first_rho_mapping_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(analytic_first_rho_mapping_buffer_);
            analytic_first_rho_mapping_buffer_ = handles::INVALID_RESOURCE;
        }
        if (analytic_first_rho_audit_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(analytic_first_rho_audit_buffer_);
            analytic_first_rho_audit_buffer_ = handles::INVALID_RESOURCE;
        }
    }
    first_surface_requested_ = false;
    first_surface_enabled_ = false;
    first_zero_cross_requested_ = false;
    first_zero_cross_enabled_ = false;
    first_native_normal_requested_ = false;
    first_native_normal_enabled_ = false;
    first_native_normal_positions_ = handles::INVALID_RESOURCE;
    first_native_normal_elements_ = handles::INVALID_RESOURCE;
    first_native_normal_instances_ = handles::INVALID_RESOURCE;
    first_native_normal_materials_ = handles::INVALID_RESOURCE;
    first_native_normal_texture_ = handles::INVALID_RESOURCE;
    first_native_normal_sampler_ = handles::INVALID_SAMPLER;
    first_native_normal_audit_bytes_ = 0;
    analytic_first_rho_requested_ = false;
    analytic_first_rho_enabled_ = false;
    analytic_first_rho_apply_ = false;
    analytic_first_position_apply_ = false;
    analytic_first_rho_audit_ = false;
    analytic_sc_first_audit_ = false;
    analytic_first_rho_prefix_ = false;
    analytic_first_rho_positions_ = handles::INVALID_RESOURCE;
    analytic_first_rho_elements_ = handles::INVALID_RESOURCE;
    analytic_first_rho_instances_ = handles::INVALID_RESOURCE;
    analytic_first_rho_materials_ = handles::INVALID_RESOURCE;
    analytic_first_rho_albedo_texture_ = handles::INVALID_RESOURCE;
    analytic_first_rho_albedo_sampler_ = handles::INVALID_SAMPLER;
    analytic_first_rho_audit_bytes_ = 0;
    device_ = nullptr;

    // No explicit GPU resource destruction needed -- handles are POD types
    // managed by the RHI device's garbage collector.
}

const math::v4* LumenDDGIPass::GetRelocatedOriginsCpu(u32& count, u32& version) {
    count = 0;
    version = relocated_origins_cpu_version_;
    if (relocated_origins_cpu_done_) {
        count = (u32)relocated_origins_cpu_.size();
        return relocated_origins_cpu_.data();
    }
    // 前 60 帧缓冲尚未被稳态写入（独立提交先于本帧 render graph 执行，读到
    // 的总是上一帧结果）——与 DDGI_ORIGIN_STATS 的触发时机同一理由。
    if (exec_frame_index_ < 60 || relocated_origins_ == handles::INVALID_RESOURCE) {
        static bool gateLogged = false;
        if (!gateLogged && exec_frame_index_ > 90u) {
            gateLogged = true;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[LumenDDGI] RelocatedOrigins CPU readback gated: frame="
                << exec_frame_index_ << " buffer="
                << (relocated_origins_ != handles::INVALID_RESOURCE ? "ok" : "INVALID")
                << std::endl;
        }
        return nullptr;
    }
    const u32 total = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    if (total == 0) return nullptr;

    BufferDesc bd{};
    bd.size = (u64)total * sizeof(math::v4);
    bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
    bd.memoryUsage = GPUMemoryUsage::Readback;
    auto stage = device_->CreateBuffer(bd);
    if (stage == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] RelocatedOrigins readback: stage buffer create failed" << std::endl;
        return nullptr;
    }
    auto ch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* c = rhi::GetCommandBuffer(ch);
    if (!c) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] RelocatedOrigins readback: command buffer unavailable" << std::endl;
        device_->DestroyBuffer(stage);
        return nullptr;
    }
    c->Begin();
    c->CopyBuffer(relocated_origins_, stage, 0, 0, bd.size);
    c->End(); c->Submit(); c->WaitForCompletion();
    device_->DestroyCommandBuffer(ch);

    const auto* data = static_cast<const math::v4*>(device_->MapBuffer(stage, 0, bd.size));
    if (!data) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] RelocatedOrigins readback: map failed" << std::endl;
        device_->DestroyBuffer(stage);
        return nullptr;
    }
    relocated_origins_cpu_.assign(data, data + total);
    device_->UnmapBuffer(stage);
    device_->DestroyBuffer(stage);

    relocated_origins_cpu_done_ = true;
    ++relocated_origins_cpu_version_;
    count = total;
    version = relocated_origins_cpu_version_;
    PRIMAL_LOG_STREAM(Error, "Render")
        << "[LumenDDGI] RelocatedOrigins CPU readback done (" << total
        << " probes, v" << version << ") — probe-sphere viz state colors live"
        << std::endl;
    return relocated_origins_cpu_.data();
}

// ============================================================================
// Private helper methods
// ============================================================================

void LumenDDGIPass::CreateDescriptorSetLayouts() {
    // --- Trace: platform-branched ---
    // Dawn/Vulkan (WGSL→SPIR-V): sequential 0..9, no texture/buffer collision, includes gbuffer_albedo for Mode 11.
    // Metal: texture/buffer namespaces overlap (idiomatic Metal), includes Surface Cache bindings.
    {
        const auto platform = device_->GetPlatform();
        const bool useFlatBindings = (platform == rhi::RHIPlatform::Dawn ||
                                      platform == rhi::RHIPlatform::Vulkan ||
                                      platform == rhi::RHIPlatform::D3D12);
        if (useFlatBindings) {
            // Dawn/Vulkan: sequential engine bindings 0..9 matching WGSL
            // (no texture/buffer collision -> no silent remap)
            DescriptorSetLayoutBinding traceBindings[] = {
                {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
                {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
                {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
                {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev frame color
                {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
                {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
                {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data (read_write)
                {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list (read)
                {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance_history (read)
                {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // gbuffer_albedo (Mode 11)
            };
            traceBindings[0].is3D = true;
            traceBindings[1].is3D = true;
            traceBindings[2].is3D = true;
            // SDF cascades are R32Float — WebGPU classifies that as UnfilterableFloat.
            // Layout's sampleType must match or CreateBindGroup validation fails.
            traceBindings[0].unfilterableFloat = true;
            traceBindings[1].unfilterableFloat = true;
            traceBindings[2].unfilterableFloat = true;
            traceBindings[7].readonly = true;
            traceBindings[8].readonly = true;
            DescriptorSetLayoutDesc layoutDesc{10, traceBindings};
            trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        } else {
            // Metal: 3 SDF textures + 1 (prev_color OR lighting_atlas) + 2 UBO + 3 SSBO + 2 SC buffers.
            // SC params (atlas_size, lookup_count) are encoded in DDGIVolumeData at buffer(1).
            DescriptorSetLayoutBinding traceBindings[] = {
                // Textures (sampled) — SDF cascades + Surface Cache lighting atlas / prev_color
                {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
                {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
                {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
                {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev_color (non-SC) or lighting_atlas (SC)
                // Buffers (separate Metal namespace)
                {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
                {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData (includes SC params)
                {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data
                {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list
                {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance_history (read)
                {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // SC card lookups
                {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // SC card data
            };
            traceBindings[0].is3D = true;
            traceBindings[1].is3D = true;
            traceBindings[2].is3D = true;
            traceBindings[3].readonly = true;  // probe_update_list
            traceBindings[4].readonly = true;  // irradiance_history
            DescriptorSetLayoutDesc layoutDesc{11, traceBindings};
            trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        }
    }

    // --- Irradiance: 2 UBO + 3 SSBO (ray data + irradiance history + irradiance output) + 1 SSBO (update list) + 1 SSBO (confidence) + 1 SSBO (openSky) ---
    {
        DescriptorSetLayoutBinding irradianceBindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance history buffer (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance output buffer (read_write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list (read)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // confidence buffer
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // GI 根治 Phase 3.1: openSky 统计 (read_write)
        };
        irradianceBindings[2].readonly = true;  // ray_buffer: var<storage, read>
        irradianceBindings[3].readonly = true;  // irradiance_history: var<storage, read>
        irradianceBindings[5].readonly = true;  // probe_update_list: var<storage, read>
        // irradianceBindings[4] (irradiance_output) stays read_write.
        DescriptorSetLayoutDesc layoutDesc{8, irradianceBindings};
        irradiance_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Depth: 2 UBO + 3 SSBO (ray data + depth history + depth output) + 1 SSBO (update list) ---
    // Buffer-based: no texture3D, uses storage buffer for Apple Silicon performance
    {
        DescriptorSetLayoutBinding depthBindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // depth history buffer (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // depth output buffer (read_write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list (read)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // Vulkan depth-only distances
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // optional provenance history
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // optional provenance output
        };
        depthBindings[2].readonly = true;  // ray_buffer: var<storage, read>
        depthBindings[3].readonly = true;  // depth_history: var<storage, read>
        depthBindings[5].readonly = true;  // probe_update_list: var<storage, read>
        // depthBindings[4] (depth_output) stays read_write.
        depthBindings[6].readonly = true;
        depthBindings[7].readonly = true;
        DescriptorSetLayoutDesc layoutDesc{(device_->GetPlatform() == RHIPlatform::Vulkan ||
                                            device_->GetPlatform() == RHIPlatform::D3D12) ?
            (depth_provenance_enabled_?9u:7u) : 6u, depthBindings};
        depth_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- SDF Trace: texture3D-only + minimal buffer reads ---
    // Apple Silicon stable: only 4 texture3D reads per thread, no Surface Cache buffer mixing.
    // Vulkan: hand-written GLSL split-pass (DDGITraceSDF.comp) with FLAT bindings.
    // Dawn: no split-pass entry — pipeline stays INVALID (unified entry only).
    const bool isVulkanPlatform = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan ||
                                   device_->GetPlatform() == rhi::RHIPlatform::D3D12);
    const bool skipSplitPass = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
    if (isVulkanPlatform) {
        // Flat bindings — no texture/buffer namespace overlap.
        DescriptorSetLayoutBinding sdfTraceBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_distance_buffer (write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probeUpdateList (read)
            // T4.3 per-mesh detail SDF: refined normals + 4 slot textures + table UBO
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_normal_buffer (write)
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // detail slot 0
            {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // detail slot 1
            {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // detail slot 2
            {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // detail slot 3
            {11, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // detail slot table
            // 2026-09-17 P0：重定位后有效起点（writeonly，绝对 probeIdx）。
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // relocated origins (write)
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {15, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {16, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        sdfTraceBindings[0].is3D = true;
        sdfTraceBindings[1].is3D = true;
        sdfTraceBindings[2].is3D = true;
        sdfTraceBindings[0].unfilterableFloat = true;
        sdfTraceBindings[1].unfilterableFloat = true;
        sdfTraceBindings[2].unfilterableFloat = true;
        sdfTraceBindings[5].readonly = true;
        for (u32 bi = 7; bi <= 10; ++bi) {   // R16_UNorm 3D — same flags as cascades
            sdfTraceBindings[bi].is3D = true;
            sdfTraceBindings[bi].unfilterableFloat = true;
        }
        for (u32 bi=13; bi<17; ++bi) sdfTraceBindings[bi].readonly=true;
        DescriptorSetLayoutDesc layoutDesc{measured_geometry_requested_ ? 17u : 13u, sdfTraceBindings};
        sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);

        // DDGI_FIRST_SURFACE=1 and DDGI_FIRST_ZERO_CROSS=1 share this
        // first-surface resource contract.  The incumbent 13-binding layout
        // above is intentionally retained for every default run and for
        // rejected opt-ins; zero-cross gets a separate pipeline/module below.
        if (first_surface_requested_ || first_zero_cross_requested_) {
            DescriptorSetLayoutBinding firstSurfaceBindings[] = {
                {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},
                {11, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {15, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            };
            for (u32 bi = 0; bi < 3; ++bi) {
                firstSurfaceBindings[bi].is3D = true;
                firstSurfaceBindings[bi].unfilterableFloat = true;
            }
            for (u32 bi = 7; bi <= 10; ++bi) {
                firstSurfaceBindings[bi].is3D = true;
                firstSurfaceBindings[bi].unfilterableFloat = true;
            }
            firstSurfaceBindings[5].readonly = true;
            firstSurfaceBindings[13].readonly = true;
            firstSurfaceBindings[14].readonly = true;
            first_surface_sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(
                DescriptorSetLayoutDesc{16, firstSurfaceBindings});

            // Native-normal child variant: preserve bindings 0..15 exactly and
            // append only the borrowed GPUDrivenDraw sources, the original
            // triangle mapping, and the independent 24-vec4 audit SSBO.
            if (first_native_normal_requested_) {
                DescriptorSetLayoutBinding firstNativeBindings[] = {
                    {0,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {1,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {2,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {3,  DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                    {4,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {5,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {6,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {7,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {8,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {9,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {10, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {11, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                    {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {15, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {16, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {17, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {18, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {19, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {20, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                    {21, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                    {22, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr},
                    {23, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                };
                for (u32 bi = 0; bi < 3; ++bi) {
                    firstNativeBindings[bi].is3D = true;
                    firstNativeBindings[bi].unfilterableFloat = true;
                }
                for (u32 bi = 7; bi <= 10; ++bi) {
                    firstNativeBindings[bi].is3D = true;
                    firstNativeBindings[bi].unfilterableFloat = true;
                }
                firstNativeBindings[5].readonly = true;
                firstNativeBindings[13].readonly = true;
                firstNativeBindings[14].readonly = true;
                for (u32 bi = 16; bi <= 22; ++bi)
                    firstNativeBindings[bi].readonly = true;
                firstNativeBindings[21].isArray = true;
                first_native_normal_sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(
                    DescriptorSetLayoutDesc{24, firstNativeBindings});
            }
        }
    } else if (!skipSplitPass) {
        DescriptorSetLayoutBinding sdfTraceBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_distance_buffer
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probeUpdateList
            // T4.3 (Metal overlapping namespaces): buffer(4) normals, buffer(5) table
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_normal_buffer (write)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // detail slot table
            // texture(3..6) = detail slots
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            // 2026-09-17 P0：relocated origins（Metal buffer(6)，与 Vulkan flat 12 对等）
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // relocated origins (write)
        };
        // R32Float 3D cascades must be declared as 3D + UnfilterableFloat on Dawn
        // (see trace_set_layout_ block above). Without these flags, CreateBindGroup
        // validation fails every frame and the pipeline collapses to [Invalid],
        // leaving Mode 11 a solid color.
        sdfTraceBindings[0].is3D = true;
        sdfTraceBindings[1].is3D = true;
        sdfTraceBindings[2].is3D = true;
        sdfTraceBindings[0].unfilterableFloat = true;
        sdfTraceBindings[1].unfilterableFloat = true;
        sdfTraceBindings[2].unfilterableFloat = true;
        sdfTraceBindings[5].readonly = true;  // probe_update_list: var<storage, read>
        for (u32 bi = 8; bi <= 11; ++bi) {
            sdfTraceBindings[bi].is3D = true;
            sdfTraceBindings[bi].unfilterableFloat = true;
        }
        DescriptorSetLayoutDesc layoutDesc{13, sdfTraceBindings};
        sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Finalize: buffer-only + texture2D (no texture3D) ---
    if (isVulkanPlatform) {
        // Flat bindings.
        DescriptorSetLayoutBinding finalizeBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // lighting_atlas (SC fallback)
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_distance_buffer (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probeUpdateList (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray_buffer (write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // card_lookups (read)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // card_data (read)
            {7, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // FinalizeViewData (272B)
            {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev_frame_color
            {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // gbuffer_albedo
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance_history (multi-bounce)
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_offsets (T1.3)
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_cards (T1.3)
            {13, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SC depth atlas (T3.4 D3)
            {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_normal_buffer (T4.3, read)
            {15, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // current depth
            {16, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // radiance depth
            // 2026-09-17 P0：本帧 TraceSDF 发布的重定位后有效起点（readonly）
            {17, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // relocated origins (read)
        };
        finalizeBindings[2].readonly = true;
        finalizeBindings[3].readonly = true;
        finalizeBindings[5].readonly = true;
        finalizeBindings[6].readonly = true;
        finalizeBindings[10].readonly = true;
        finalizeBindings[11].readonly = true;
        finalizeBindings[12].readonly = true;
        finalizeBindings[14].readonly = true;
        finalizeBindings[17].readonly = true;
        DescriptorSetLayoutDesc layoutDesc{18, finalizeBindings};
        finalize_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);

        // Analytic-first-rho Finalize variant.  Bindings 0..17 intentionally
        // mirror the incumbent flat contract; 18..27 are candidate-only
        // geometry/material/audit resources.  The candidate is created only
        // for the exact opt-in flag or diagnostic PREFIX, so the default
        // descriptor layout and bytecode stay unchanged.
        if (analytic_first_rho_requested_) {
            DescriptorSetLayoutBinding analyticBindings[] = {
                {0,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {1,  DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {2,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {3,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {4,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {5,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {6,  DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {7,  DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {8,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {9,  DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {13, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {15, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {16, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {17, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {18, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {19, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {20, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {21, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {22, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {23, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {24, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {25, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {26, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr},
                {27, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            };
            for (u32 bi : {0u, 8u, 9u, 13u, 15u, 16u, 25u}) {
                analyticBindings[bi].isArray = (bi == 25u);
            }
            for (u32 bi : {2u, 3u, 5u, 6u, 10u, 11u, 12u, 14u, 17u,
                           18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u})
                analyticBindings[bi].readonly = true;
            analytic_first_rho_finalize_set_layout_ = device_->CreateDescriptorSetLayout(
                DescriptorSetLayoutDesc{28, analyticBindings});
        }
    } else if (!skipSplitPass) {
        DescriptorSetLayoutBinding finalizeBindings[] = {
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // lighting_atlas
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // hit_distance_buffer
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probeUpdateList
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray_buffer
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // card_lookups
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // card_data
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // sc_grid_offsets (T1.3)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // sc_grid_cards (T1.3)
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev_frame_color (T3.1)
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // gbuffer_albedo (T3.1)
            {9, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // ViewProjection (T3.1)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance_history (T3.1)
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SC depth atlas (T3.4 D3 plane test)
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_normal_buffer (T4.3, read)
            // 2026-09-17 P0：relocated origins（Metal buffer(12)，与 Vulkan flat 17 对等）
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // relocated origins (read)
        };
        finalizeBindings[7].readonly = true;
        finalizeBindings[8].readonly = true;
        finalizeBindings[10].readonly = true;
        finalizeBindings[14].readonly = true;
        finalizeBindings[15].readonly = true;  // relocated origins (buffer 12)
        DescriptorSetLayoutDesc layoutDesc{16, finalizeBindings};
        finalize_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void LumenDDGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        // T8.9b diagnostic override: DDGI_PATH_DEBUG=1 prefers the *_pathdbg
        // bytecode (path-id tags baked via -DDGI_PATH_DEBUG) for the finalize
        // kernel so ray-path A/B never races the POST_BUILD shader staging
        // copy. Repository keeps both .comp.spv files side by side.
        std::string effectiveName = name;
        if (depth_provenance_enabled_ && std::string(name)=="DDGIUpdateDepth")
            effectiveName="DDGIUpdateDepth_provenance";
        if (std::getenv("DDGI_PATH_DEBUG") && std::string(name) == "DDGITraceFinalize") {
            effectiveName = "DDGITraceFinalize_pathdbg";
        }
        if (std::getenv("DDGI_PATH_DEBUG") &&
            std::string(name) == "DDGITraceFinalize_analytic_first_rho") {
            effectiveName = "DDGITraceFinalize_analytic_first_rho_pathdbg";
        }
        // 2026-09-17 归因 A/B：DDGI_LEGACY_CONSUMERS=1 → 消费端旧原格点行为
        // 变体（build_spv.sh 以 -DDDGI_LEGACY_CONSUMERS 编译；缓冲仍照常写）。
        if (std::getenv("DDGI_LEGACY_CONSUMERS") &&
            (std::string(name) == "DDGITraceFinalize")) {
            effectiveName = "DDGITraceFinalize_legacy";
        }
        if (measured_geometry_requested_ && std::string(name)=="DDGITraceSDF")
            effectiveName="DDGITraceSDF_measured_geometry";
        if (measured_geometry_requested_ && std::string(name)=="DDGITraceFinalize")
            effectiveName=std::getenv("DDGI_PATH_DEBUG") ? "DDGITraceFinalize_measured_geometry_pathdbg" : "DDGITraceFinalize_measured_geometry";
        auto code = LoadShaderBytecode(effectiveName.c_str(), device_);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    // Compile shaders
    auto traceShader = CompileShader("DDGITraceRays", "ddgi_trace_rays");
    auto irradianceShader = CompileShader("DDGIUpdateIrradiance", "ddgi_update_irradiance");
    auto depthShader = CompileShader("DDGIUpdateDepth", "ddgi_update_depth");

    // Split-pass shaders (ddgi_trace_sdf / ddgi_trace_finalize):
    //   Metal:   both entries live in DDGITraceRays.metal.
    //   Vulkan:  hand-written GLSL ports in separate files (DDGITraceSDF.comp /
    //            DDGITraceFinalize.comp → .comp.spv). Entry names identical.
    //   Dawn:    no split-pass entry — unified ddgi_trace_rays only.
    // D3D12 与 Vulkan 同形：split-pass cso 镜像（DDGITraceSDF.comp.cso /
    // DDGITraceFinalize.comp.cso），flat binding 布局一致。
    const bool isVulkanPlatform = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan ||
                                   device_->GetPlatform() == rhi::RHIPlatform::D3D12);
    const bool isMetalOrVkSplit = (device_->GetPlatform() == rhi::RHIPlatform::Metal) || isVulkanPlatform;
    ShaderHandle sdfTraceShader = handles::INVALID_SHADER;
    ShaderHandle finalizeShader = handles::INVALID_SHADER;
    ShaderHandle firstSurfaceSdfTraceShader = handles::INVALID_SHADER;
    ShaderHandle firstZeroCrossSdfTraceShader = handles::INVALID_SHADER;
    ShaderHandle firstNativeNormalSdfTraceShader = handles::INVALID_SHADER;
    ShaderHandle analyticFirstRhoFinalizeShader = handles::INVALID_SHADER;
    if (device_->GetPlatform() == rhi::RHIPlatform::Metal) {
        sdfTraceShader = CompileShader("DDGITraceRays", "ddgi_trace_sdf");
        finalizeShader = CompileShader("DDGITraceRays", "ddgi_trace_finalize");
    } else if (isVulkanPlatform) {
        sdfTraceShader = CompileShader("DDGITraceSDF", "ddgi_trace_sdf");
        finalizeShader = CompileShader("DDGITraceFinalize", "ddgi_trace_finalize");
        if (analytic_first_rho_requested_) {
            analyticFirstRhoFinalizeShader = CompileShader(
                "DDGITraceFinalize_analytic_first_rho", "ddgi_analytic_first_rho");
        }
        if (first_surface_requested_) {
            firstSurfaceSdfTraceShader = CompileShader(
                "DDGITraceSDF_firstsurface", "ddgi_trace_sdf");
        }
        if (first_zero_cross_requested_) {
            firstZeroCrossSdfTraceShader = CompileShader(
                "DDGITraceSDF_first_zero_cross", "ddgi_trace_sdf");
        }
        if (first_native_normal_requested_) {
            firstNativeNormalSdfTraceShader = CompileShader(
                "DDGITraceSDF_firstnative", "ddgi_trace_sdf");
        }
    }
    first_surface_sdf_trace_shader_ = firstSurfaceSdfTraceShader;
    first_zero_cross_sdf_trace_shader_ = firstZeroCrossSdfTraceShader;
    first_native_normal_sdf_trace_shader_ = firstNativeNormalSdfTraceShader;
    analytic_first_rho_finalize_shader_ = analyticFirstRhoFinalizeShader;

    if (traceShader == handles::INVALID_SHADER ||
        irradianceShader == handles::INVALID_SHADER ||
        depthShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] Shader compilation failed" << std::endl;
        return;
    }

    // Create pipeline layouts
    // (Split-pass layouts exist on Metal AND Vulkan now; Dawn only.)
    const bool skipSplitPass = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &trace_set_layout_;
        trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &irradiance_set_layout_;
        irradiance_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &depth_set_layout_;
        depth_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (!skipSplitPass && sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &sdf_trace_set_layout_;
        sdf_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if ((first_surface_requested_ || first_zero_cross_requested_) &&
        first_surface_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &first_surface_sdf_trace_set_layout_;
        first_surface_sdf_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (first_native_normal_requested_ &&
        first_native_normal_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &first_native_normal_sdf_trace_set_layout_;
        first_native_normal_sdf_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (!skipSplitPass && finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &finalize_set_layout_;
        finalize_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (analytic_first_rho_requested_ &&
        analytic_first_rho_finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &analytic_first_rho_finalize_set_layout_;
        analytic_first_rho_finalize_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipelines
    // Trace uses threadGroupSize (64,1,1) for ray-level parallelism
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
        if (trace_pipeline_ == handles::INVALID_PIPELINE) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[LumenDDGI] trace_pipeline_ creation FAILED (shader compile or layout mismatch)" << std::endl;
        }
    }
    // SDF trace (split pass 1) + Finalize (split pass 2) — Apple Silicon optimization
    // that splits texture3D-only tracing from buffer-only card search.
    //
    // SKIP ON DAWN: the split-pass WGSL entry points (ddgi_trace_sdf,
    // ddgi_trace_finalize) were never ported from Metal. Compiling the pipeline
    // against the existing ddgi_trace_rays module produces a non-null but
    // [Invalid] pipeline (Dawn returns invalid-but-non-null on validation
    // failure). The `!= INVALID_PIPELINE` check at the dispatch site then
    // passes, BindComputePipeline([Invalid]) poisons the command buffer, and
    // every subsequent encode — including the meshlet GBuffer pass — becomes
    // [Invalid] too, collapsing Mode 11 to solid clear color.
    //
    // Leaving both as INVALID_PIPELINE routes Dawn through the legacy
    // trace_pipeline_ path below, which uses ddgi_trace_rays (entry that
    // exists in WGSL).
    const bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
    const bool isVulkan = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
    if (!isDawn) {
        // SDF trace (split pass 1): texture3D-only, (8,8,1) threadgroup
        // (Vulkan now builds split-pass too — GLSL ports + flat layouts.)
        if (sdfTraceShader != handles::INVALID_SHADER &&
            sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = sdfTraceShader;
            pipeDesc.layout = sdf_trace_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
        }
        if (first_surface_requested_ &&
            firstSurfaceSdfTraceShader != handles::INVALID_SHADER &&
            first_surface_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = firstSurfaceSdfTraceShader;
            pipeDesc.layout = first_surface_sdf_trace_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            first_surface_sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
            if (first_surface_sdf_trace_pipeline_ == handles::INVALID_PIPELINE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_FIRST_SURFACE] candidate pipeline creation failed"
                    << std::endl;
            }
        } else if (first_surface_requested_) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_SURFACE] candidate shader/layout unavailable"
                << std::endl;
        }
        if (first_zero_cross_requested_ &&
            firstZeroCrossSdfTraceShader != handles::INVALID_SHADER &&
            first_surface_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = firstZeroCrossSdfTraceShader;
            pipeDesc.layout = first_surface_sdf_trace_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            first_zero_cross_sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
            if (first_zero_cross_sdf_trace_pipeline_ == handles::INVALID_PIPELINE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_FIRST_ZERO_CROSS] candidate pipeline creation failed"
                    << std::endl;
            }
        } else if (first_zero_cross_requested_) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_ZERO_CROSS] candidate shader/layout unavailable"
                << std::endl;
        }
        if (first_native_normal_requested_ &&
            firstNativeNormalSdfTraceShader != handles::INVALID_SHADER &&
            first_native_normal_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = firstNativeNormalSdfTraceShader;
            pipeDesc.layout = first_native_normal_sdf_trace_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            first_native_normal_sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
            if (first_native_normal_sdf_trace_pipeline_ == handles::INVALID_PIPELINE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_FIRST_NATIVE_NORMAL] candidate pipeline creation failed"
                    << std::endl;
            }
        } else if (first_native_normal_requested_) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_NATIVE_NORMAL] candidate shader/layout unavailable"
                << std::endl;
        }
        // Finalize (split pass 2): buffer-only, (8,8,1) threadgroup
        if (finalizeShader != handles::INVALID_SHADER &&
            finalize_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = finalizeShader;
            pipeDesc.layout = finalize_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            finalize_pipeline_ = device_->CreateComputePipeline(pipeDesc);
        }
        if (analytic_first_rho_requested_ &&
            analyticFirstRhoFinalizeShader != handles::INVALID_SHADER &&
            analytic_first_rho_finalize_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = analyticFirstRhoFinalizeShader;
            pipeDesc.layout = analytic_first_rho_finalize_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            analytic_first_rho_finalize_pipeline_ = device_->CreateComputePipeline(pipeDesc);
            if (analytic_first_rho_finalize_pipeline_ == handles::INVALID_PIPELINE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_ANALYTIC_FIRST_RHO] candidate pipeline creation failed"
                    << std::endl;
            }
        } else if (analytic_first_rho_requested_) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_ANALYTIC_FIRST_RHO] candidate shader/layout unavailable"
                << std::endl;
        }
    }
    // Irradiance update uses (8,8,1) — cooperative, one group per probe
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = irradianceShader;
        pipeDesc.layout = irradiance_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        irradiance_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // Depth update uses (8,8,1) — cooperative, one group per probe
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = depthShader;
        pipeDesc.layout = depth_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        depth_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{trace_set_layout_};
            trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{irradiance_set_layout_};
            irradiance_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{depth_set_layout_};
            depth_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{sdf_trace_set_layout_};
            sdf_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if ((first_surface_requested_ || first_zero_cross_requested_) &&
            first_surface_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{first_surface_sdf_trace_set_layout_};
            first_surface_sdf_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (first_native_normal_requested_ &&
            first_native_normal_sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{first_native_normal_sdf_trace_set_layout_};
            first_native_normal_sdf_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{finalize_set_layout_};
            finalize_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (analytic_first_rho_requested_ &&
            analytic_first_rho_finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{analytic_first_rho_finalize_set_layout_};
            analytic_first_rho_finalize_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

// T1.3 (C5): 3-slot readback ring, one depth field per slot (8 MB at the
// default 16k-probe grid). Bandwidth cost ~0.1 ms/frame of GPU copy; buys
// zero CPU-GPU sync points in the priority scheduler.
void LumenDDGIPass::CreateDepthReadbackRing() {
    static constexpr u32 kFloatsPerProbe = 128;   // [mean64 | var64]
    const u64 probeCount = (u64)params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    const u64 bytes = probeCount * kFloatsPerProbe * sizeof(float);
    if (!params_.far_update_budget && !depth_readback_ring_.initialize(device_, bytes, 3)) {
        PRIMAL_LOG_STREAM(Warn, "Render")
            << "[LumenDDGI] depth readback ring init failed — scheduler falls back to distance/age priority."
            << std::endl;
    }

    // T1.5 (R4): indirect dispatch count buffer (Vulkan layout: 3×u32
    // group counts; Metal dispatchThreadgroupsWithIndirectBuffer reads the
    // same 3×u32 threadgroup layout; Dawn dispatchWorkgroupsIndirect too).
    {
        BufferDesc desc{};
        desc.size = 16;   // 3×u32 + pad
        desc.type = BufferType::Structured;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(ResourceUsage::IndirectArg) |
                         static_cast<u32>(BufferUsageFlags::TransferDst);
        probe_dispatch_count_buffer_ = device_->CreateBuffer(desc);
    }
}

void LumenDDGIPass::CreateConstantBuffers() {
    auto CreateCBs = [&](ResourceHandle (&cbs)[3], u64 size) {
        for (int i = 0; i < 3; i++) {
            BufferDesc desc{};
            desc.size = size;
            desc.type = BufferType::Constant;
            desc.usage = GPUMemoryUsage::Dynamic;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            desc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
            cbs[i] = device_->CreateBuffer(desc);
        }
    };

    CreateCBs(global_cb_, 512);          // GlobalShaderData (432 bytes, padded)
    CreateCBs(volume_cb_, 512);          // DDGIVolumeData
    CreateCBs(finalize_vp_cb_, sizeof(FinalizeViewData));
    // T4.3: detail-SDF slot table (16B header + 4 × 160B slots = 656B).
    // Always bound; header count=0 makes the shader skip refinement when the
    // feature is off — no dummy CB needed.
    CreateCBs(detail_table_cb_, 656);
}

void LumenDDGIPass::GetVolumeAABB(math::v3& out_min, math::v3& out_max) const {
    // Probe positions sit at origin + cell*spacing; pad one cell so rays
    // originating just outside the grid still fall inside the refinement
    // region.
    const math::v3 extent{
        (float)(params_.probe_count_x - 1) * params_.probe_spacing,
        (float)(params_.probe_count_y - 1) * params_.probe_spacing,
        (float)(params_.probe_count_z - 1) * params_.probe_spacing,
    };
    // 原写法 math::v3{probe_spacing}：Apple simd 的单标量花括号初始化只设 x
    // （y/z 补 0），不是三轴广播。按"Mac 行为零变化"保留该语义并写明分量；
    // 若本意是三轴外扩，应改为 math::splat3（会改变 Mac 现有结果，待定）。
    out_min = probe_origin_ - math::v3{ params_.probe_spacing, 0.0f, 0.0f };
    out_max = probe_origin_ + extent + math::v3{ params_.probe_spacing, 0.0f, 0.0f };
}

void LumenDDGIPass::CreateProbeTextures() {
    u32 nx = params_.probe_count_x;
    u32 ny = params_.probe_count_y;
    u32 nz = params_.probe_count_z;

    // Irradiance storage buffers: each probe stores 9 SH3 coefficients (float3 each)
    u32 totalProbes = nx * ny * nz;
    // 9 SH coeffs per probe, stored as vec4 (16B each) to match GIGather's
    // vec4 irradianceBuffer[] declaration. Total = 9 * 16 = 144 bytes/probe.
    u64 irradianceSize = (u64)totalProbes * 9 * sizeof(math::v4);

    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = irradianceSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = totalProbes * 9;
        desc.structured.elementStride = sizeof(math::v4);
        irradiance_buffers_[i] = device_->CreateBuffer(desc);
        // TEMP diagnosis: allocation audit — BIND7 caught a 20-byte buffer at
        // compute slot 7; verify what the DDGI pass actually got.
        PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_ALLOC] irradiance_buffers_[" << i << "] handle="
                  << irradiance_buffers_[i] << " size=" << desc.size
                  << (irradiance_buffers_[i] == rhi::handles::INVALID_RESOURCE
                      ? " INVALID!" : "") << std::endl;

        // NOTE: No zero-init here — InitializeProbesFromStatic() will fill
        // with static bake data or sky estimate after all buffers are created.
    }

    // Depth buffers: octahedral 8x8 depth map per probe
    // Layout per probe: [mean0..mean63, variance0..variance63] = 128 floats
    // Apple Silicon: buffer load/store is much faster than texture3D sampling
    static constexpr u32 DDGI_DEPTH_RES = 8;
    static constexpr u32 DDGI_DEPTH_TEXELS = DDGI_DEPTH_RES * DDGI_DEPTH_RES; // 64
    static constexpr u32 DDGI_FLOATS_PER_PROBE = DDGI_DEPTH_TEXELS * 2; // 128
    u64 depthSize = (u64)totalProbes * DDGI_FLOATS_PER_PROBE * sizeof(float);
    float depthMeanInit = ray_sample_mode_ == 7u ? -1.0f : params_.ray_max_distance;  // mode7 starts unmeasured; legacy modes retain open initialization
    float depthVarInit  = ray_sample_mode_ == 7u ? 0.0f : params_.probe_spacing * params_.probe_spacing * 0.25f; // non-zero avoids Chebyshev singularity

    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = depthSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = totalProbes * DDGI_FLOATS_PER_PROBE;
        desc.structured.elementStride = sizeof(float);
        depth_buffers_[i] = device_->CreateBuffer(desc);

        // Initialize with sensible defaults
        float* mapped = static_cast<float*>(device_->MapBuffer(depth_buffers_[i]));
        if (mapped) {
            for (u32 p = 0; p < totalProbes; ++p) {
                float* probe = mapped + p * DDGI_FLOATS_PER_PROBE;
                // First 64 floats: depth mean — fill with ray_max_distance (open)
                for (u32 j = 0; j < DDGI_DEPTH_TEXELS; ++j) probe[j] = depthMeanInit;
                // Next 64 floats: depth variance — non-zero to avoid Chebyshev singularity
                for (u32 j = DDGI_DEPTH_TEXELS; j < DDGI_FLOATS_PER_PROBE; ++j) probe[j] = depthVarInit;
            }
            device_->UnmapBuffer(depth_buffers_[i]);
        }
    }

    CreateDepthReadbackRing();

    if (depth_provenance_enabled_) {
        for (auto& buffer:depth_provenance_buffers_) {
            BufferDesc desc{};desc.size=u64(totalProbes)*64u*8u*sizeof(u32);
            desc.type=BufferType::Structured;
            desc.usage=desc.memoryUsage=GPUMemoryUsage::Dynamic;
            desc.bindFlags=u32(BufferUsageFlags::Storage);
            desc.structured.elementCount=totalProbes*64u*2u;
            desc.structured.elementStride=sizeof(math::v4);
            buffer=device_->CreateBuffer(desc);
            if (buffer!=handles::INVALID_RESOURCE) {
                if (auto* p=device_->MapBuffer(buffer)) {
                    std::memset(p,0,desc.size);device_->UnmapBuffer(buffer);
                } else {
                    device_->DestroyBuffer(buffer);buffer=handles::INVALID_RESOURCE;
                }
            }
        }
    }

    // Confidence buffer (single-buffered). convergenceAge is a monotonic
    // per-probe counter (+1/30 per update); the previous triple-buffered
    // array rotated the written slot each frame, so every slot only advanced
    // every 3rd frame and GIGather (reading frame%3) saw probes that never
    // settled. One buffer keeps the counter monotonic for all consumers.
    {
        u64 confSize = (u64)totalProbes * 4 * sizeof(float);
        BufferDesc desc{};
        desc.size = confSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = totalProbes * 4;
        desc.structured.elementStride = sizeof(float);

        float initAge = (static_volume_ && static_volume_->IsLoaded() && ray_sample_mode_ != 7u) ? 0.5f : 0.0f;
        confidence_buffer_ = device_->CreateBuffer(desc);

        float* mapped = static_cast<float*>(device_->MapBuffer(confidence_buffer_));
        if (mapped) {
            for (u32 p = 0; p < totalProbes; p++) {
                mapped[p * 4 + 0] = 0.0f;     // rayHitRatio
                mapped[p * 4 + 1] = 0.0f;     // temporalStability
                mapped[p * 4 + 2] = 1.0f;     // visibilityConf (optimistic)
                mapped[p * 4 + 3] = initAge;  // convergenceAge
            }
            device_->UnmapBuffer(confidence_buffer_);
        }
    }

    // GI 根治 Phase 3.1 + 能量所有权 Phase 4a: 每探针 openSky 统计缓冲，
    // float2/probe = {escapeRatio, skyEnergyRatio}（ UpdateIrradiance 写，
    // EMA 平滑；GIGather/LightEval 消费——V2 拆 E_geo/E_sky 用 y 分量）。
    // 必须显式清零——Vulkan/堆纹理不保证零初始化（confidence 同款教训），
    // 垃圾值会让 gather 的天空分类在首帧就失真。单缓冲持久，探针逐批
    // EMA 更新。
    {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * 2u * sizeof(float);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = 2u * sizeof(float);
        opensky_buffer_ = device_->CreateBuffer(desc);
        if (float* mapped = static_cast<float*>(device_->MapBuffer(opensky_buffer_))) {
            for (u32 p = 0; p < totalProbes * 2u; p++) mapped[p] = 0.0f;
            device_->UnmapBuffer(opensky_buffer_);
        }
    }

    // Ray data storage buffer
    // Each probe fires rays_per_probe rays, each ray produces DDGIRayData (16 bytes)
    u64 rayDataSize = (u64)totalProbes * params_.rays_per_probe * sizeof(DDGIRayData);

    {
        BufferDesc desc{};
        desc.size = rayDataSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(DDGIRayData);
        ray_data_buffer_ = device_->CreateBuffer(desc);
    }

    // Hit distance buffer (intermediate between split SDF trace and finalize dispatches)
    // Must match ray_data_buffer_ sizing: totalProbes * rays_per_probe, because
    // the dispatch uses updateCount = probeCountTotal groups × 64 rays = full grid.
    {
        u32 totalRays = totalProbes * params_.rays_per_probe;
        BufferDesc desc{};
        const u32 distanceCount = totalRays + (ray_sample_mode_ == 5 ? totalProbes * 128u : 0u);
        desc.size = (u64)distanceCount * sizeof(float);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = distanceCount;
        desc.structured.elementStride = sizeof(float);
        hit_distance_buffer_ = device_->CreateBuffer(desc);

        // T4.3: parallel refined-normal buffer — xyz = world normal,
        // w = 1.0 valid / 0.0 fallback (finalize keeps -rayDir). Zero-filled
        // by the trace every frame (slot count 0 → all zeros).
        material_projection_enabled_=device_->GetPlatform()==rhi::RHIPlatform::Vulkan &&
            std::getenv("DDGI_MATERIAL_PROJECT");
        // DDGI_RAY_SOURCE_AUDIT is deliberately an exact opt-in.  The
        // diagnostic is Vulkan split-pass only; unset/0/invalid values keep
        // the allocation and UBO layout at the incumbent size.
        const char* sourceAuditEnv = std::getenv("DDGI_RAY_SOURCE_AUDIT");
        const bool splitPassAvailable = device_->GetPlatform()==rhi::RHIPlatform::Vulkan &&
            sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
            finalize_pipeline_ != handles::INVALID_PIPELINE;
        ray_source_audit_enabled_=splitPassAvailable && totalProbes <= 65535u &&
            sourceAuditEnv && std::strcmp(sourceAuditEnv, "1") == 0;
        // DDGI_QUERY_SOURCE_AUDIT is a stricter, scalar-mode-only extension of
        // the existing source audit.  Latch it before allocating the hit-
        // normal buffer so the extra tail can never be enabled without its
        // storage.  Albedo.w remains the incumbent source-audit flag.
        const char* querySourceAuditEnv = std::getenv("DDGI_QUERY_SOURCE_AUDIT");
        query_source_audit_enabled_ = ray_source_audit_enabled_ &&
            ray_sample_mode_ == 0u && querySourceAuditEnv &&
            std::strcmp(querySourceAuditEnv, "1") == 0;
        if (querySourceAuditEnv && std::strcmp(querySourceAuditEnv, "1") == 0 &&
            !query_source_audit_enabled_) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_QUERY_SOURCE_AUDIT] rejected: requires Vulkan split-pass, "
                << "DDGI_RAY_SOURCE_AUDIT=1, scalar ray mode 0, and <=65535 probes"
                << std::endl;
        }
        analytic_audit_enabled_=device_->GetPlatform()==rhi::RHIPlatform::Vulkan &&
            (std::getenv("DDGI_ANALYTIC_AUDIT") || material_projection_enabled_ ||
             ray_source_audit_enabled_);
        // Record layout:
        //   [0, N)       refined hit normal
        //   [N, 5N)      existing four analytic records (unchanged)
        //   [5N, 9N)     source audit: Trace origin/direction, Finalize
        //                 origin/direction (one vec4 per item per ray).
        //   [9N, 27N)    optional query-source audit: two headers and sixteen
        //                 actual card reads (one vec4 per item per ray).
        const u32 normalRecordMultiplier=query_source_audit_enabled_ ? 27u :
            (ray_source_audit_enabled_ ? 9u :
             (analytic_audit_enabled_ ? 5u : 1u));
        desc.size = (u64)totalRays * normalRecordMultiplier * 4 * sizeof(float);
        desc.structured.elementCount = totalRays * normalRecordMultiplier;
        desc.structured.elementStride = 4 * sizeof(float);
        hit_normal_buffer_ = device_->CreateBuffer(desc);
        if (ray_source_audit_enabled_) {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[DDGI_RAY_SOURCE_AUDIT] enabled (Vulkan split-pass); hit-normal records="
                << normalRecordMultiplier << "N, source tail starts at 5N" << std::endl;
        }
        if (query_source_audit_enabled_) {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[DDGI_QUERY_SOURCE_AUDIT] enabled; records="
                << normalRecordMultiplier << "N, query tail starts at 9N"
                << " (18 vec4/ray)" << std::endl;
        }

        // 2026-09-17 P0：重定位后有效起点（float4 per probe，绝对 probeIdx）。
        // 显式清零：w=0 表示"该 probe 尚未被 trace 发布过有效起点"，消费方
        // （finalize / gather）据此回退逻辑格点，首帧/紧预算下不读到垃圾。
        {
            BufferDesc roDesc{};
            roDesc.size = (u64)totalProbes * sizeof(math::v4);
            roDesc.type = BufferType::Structured;
            roDesc.usage = GPUMemoryUsage::Dynamic;
            roDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            roDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
            roDesc.structured.elementCount = totalProbes;
            roDesc.structured.elementStride = sizeof(math::v4);
            relocated_origins_ = device_->CreateBuffer(roDesc);
            if (relocated_origins_ != handles::INVALID_RESOURCE) {
                if (auto* mapped =
                        static_cast<math::v4*>(device_->MapBuffer(relocated_origins_))) {
                    std::memset(mapped, 0, (size_t)roDesc.size);
                    device_->UnmapBuffer(relocated_origins_);
                }
            }
        }
    }

    // Dummy Surface Cache buffers — always bound so the trace descriptor set
    // is valid even when SC is not enabled. lookup_count=0 causes the shader
    // to skip the SC loop and fall through to sky color.
    {
        BufferDesc desc{};
        desc.size = 256;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_card_lookup_buffer_ = device_->CreateBuffer(desc);
        sc_card_data_buffer_ = device_->CreateBuffer(desc);
    }

    // Dummy card-grid buffers (T1.3) — finalize statically reads the grid, so
    // bindings must stay valid when no real grid was injected. The dummy
    // describes an empty 1³ grid at the world origin whose single cell holds
    // no cards: any query outside [0,1)³ returns the outside sentinel, and a
    // query inside gets an empty [0,0) range.
    {
        uint32_t dummy_words[18] = {};
        // min_bits[3]=0, card_count=0 | max_bits[3]=0, cell_size_bits(1.0f)
        const float one = 1.0f;
        std::memcpy(&dummy_words[7], &one, 4);
        dummy_words[8] = 1;  // dims.x
        dummy_words[9] = 1;  // dims.y
        dummy_words[10] = 1; // dims.z
        dummy_words[11] = 16; // header_words
        // words 16..17 = cell offsets [0, 0]

        BufferDesc goff{};
        goff.size = sizeof(dummy_words);
        goff.type = BufferType::Structured;
        goff.usage = GPUMemoryUsage::Dynamic;
        goff.memoryUsage = GPUMemoryUsage::Dynamic;
        goff.structured.elementCount = 18;
        goff.structured.elementStride = sizeof(uint32_t);
        sc_grid_offsets_buffer_ = device_->CreateBuffer(goff);
        auto* mapped = static_cast<uint32_t*>(device_->MapBuffer(sc_grid_offsets_buffer_));
        if (mapped) {
            std::memcpy(mapped, dummy_words, sizeof(dummy_words));
            device_->UnmapBuffer(sc_grid_offsets_buffer_);
        }

        BufferDesc gcards{};
        gcards.size = 16;
        gcards.type = BufferType::Structured;
        gcards.usage = GPUMemoryUsage::Dynamic;
        gcards.memoryUsage = GPUMemoryUsage::Dynamic;
        gcards.structured.elementCount = 4;
        gcards.structured.elementStride = sizeof(uint32_t);
        sc_grid_cards_buffer_ = device_->CreateBuffer(gcards);
    }

    // Dummy depth atlas (T3.4 D3 兜底) — finalize 的 binding13(R32_Float 深度
    // 图集) 静态绑定,SC atlas 未就绪时若留 INVALID 会写出 VK_NULL_HANDLE
    // 采样图描述符(Vulkan validation 02997 + dispatch 期 binding 无效)。
    // 1×1 R32F 恒 0 深度,SC 未启用路径本就不消费该数据。
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::R32_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource;
        sc_depth_atlas_ = device_->CreateTexture(desc);
        device_->PrimeTextureLayout(sc_depth_atlas_);  // P0-B 首帧 priming（1x1 深度占位）
    }
    {
        BufferDesc desc{};
        desc.size = 256;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_params_buffer_ = device_->CreateBuffer(desc);

        // Upload zeroed params so lookup_count=0
        auto* mapped = static_cast<SurfaceCacheParams*>(device_->MapBuffer(sc_params_buffer_));
        if (mapped) {
            memset(mapped, 0, sizeof(SurfaceCacheParams));
            device_->UnmapBuffer(sc_params_buffer_);
        }
    }
}

// ============================================================================
// Initialize probes from static bake data or sky estimate fallback
// ============================================================================

// T4.1 (D8): reset convergence ages so the partial-update scheduler
// prioritizes every probe for re-initialization after a field clear.
void LumenDDGIPass::BoostProbePriority() {
    // Resetting lastUpdate to 0 puts every probe at high age urgency.
    for (auto& ps : probe_states_) {
        ps.last_update_frame = 0;
    }
}

bool LumenDDGIPass::CreateFieldResetTemplate() {
    const u64 n=u64(params_.probe_count_x)*params_.probe_count_y*params_.probe_count_z;
    reset_zero_bytes_=n*(depth_provenance_enabled_?2048u:144u);
    reset_depth_offset_=reset_zero_bytes_;
    reset_confidence_offset_=reset_depth_offset_+n*512u;
    BufferDesc desc{}; desc.size=reset_confidence_offset_+n*16u;
    desc.type=BufferType::Structured; desc.memoryUsage=GPUMemoryUsage::Dynamic;
    desc.usage=GPUMemoryUsage::Dynamic; desc.bindFlags=static_cast<u32>(BufferUsageFlags::TransferSrc);
    field_reset_template_=device_->CreateBuffer(desc);
    if(field_reset_template_==handles::INVALID_RESOURCE) return false;
    auto* bytes=static_cast<unsigned char*>(device_->MapBuffer(field_reset_template_));
    if(!bytes) return false;
    std::memset(bytes,0,desc.size);
    auto* depth=reinterpret_cast<float*>(bytes+reset_depth_offset_);
    for(u64 p=0;p<n;++p) for(u32 j=0;j<128;++j)
        depth[p*128+j]=j<64 ? (ray_sample_mode_==7u?-1.f:params_.ray_max_distance)
            : (ray_sample_mode_==7u?0.f:params_.probe_spacing*params_.probe_spacing*.25f);
    auto* confidence=reinterpret_cast<float*>(bytes+reset_confidence_offset_);
    for(u64 p=0;p<n;++p) confidence[p*4+2]=1.f; // zero age => first observation replaces EMA
    device_->UnmapBuffer(field_reset_template_);
    return true;
}

void LumenDDGIPass::RecordFieldReset(RHICommandBuffer* cmd) {
    if(!field_reset_pending_ || field_reset_template_==handles::INVALID_RESOURCE) return;
    const u64 n=u64(params_.probe_count_x)*params_.probe_count_y*params_.probe_count_z;
    // Same graphics queue: wait on prior frames' readers/writers before changing
    // all slots. Immutable upload contents never race a pending GPU read.
    cmd->MemoryBarrier(PipelineStage::AllCommands | PipelineStage::Host,PipelineStage::Transfer,
        AccessFlag::MemoryRead | AccessFlag::MemoryWrite | AccessFlag::HostWrite,
        AccessFlag::TransferRead | AccessFlag::TransferWrite);
    const auto copy=[&](ResourceHandle dst,u64 src,u64 size) {
        if(dst!=handles::INVALID_RESOURCE) cmd->CopyBuffer(field_reset_template_,dst,src,0,size);
    };
    for(u32 i=0;i<3;++i) {
        copy(irradiance_buffers_[i],0,n*144u);
        copy(depth_buffers_[i],reset_depth_offset_,n*512u);
        copy(depth_provenance_buffers_[i],0,n*2048u);
    }
    copy(confidence_buffer_,reset_confidence_offset_,n*16u);
    copy(opensky_buffer_,0,n*8u);
    copy(relocated_origins_,0,n*16u);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader | PipelineStage::Transfer,
        AccessFlag::TransferWrite,AccessFlag::ShaderRead | AccessFlag::ShaderWrite | AccessFlag::TransferRead | AccessFlag::TransferWrite);
    field_reset_pending_=false;
}

void LumenDDGIPass::InheritProbeHistory(RHICommandBuffer* cmd,u32 out,u32 history) {
    const u64 n=u64(params_.probe_count_x)*params_.probe_count_y*params_.probe_count_z;
    cmd->MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,
        AccessFlag::MemoryRead | AccessFlag::MemoryWrite,AccessFlag::TransferRead | AccessFlag::TransferWrite);
    cmd->CopyBuffer(irradiance_buffers_[history],irradiance_buffers_[out],0,0,n*144u);
    cmd->CopyBuffer(depth_buffers_[history],depth_buffers_[out],0,0,n*512u);
    if(depth_provenance_enabled_) cmd->CopyBuffer(depth_provenance_buffers_[history],depth_provenance_buffers_[out],0,0,n*2048u);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,
        AccessFlag::TransferWrite,AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
}

void LumenDDGIPass::PublishFarUpdateAudit(RHICommandBuffer* cmd,u32 slot) {
    const auto& a=far_update_audit_;
    const char* env=std::getenv("DDGI_FAR_UPDATE_AUDIT");
    if(env && std::strcmp(env,"1")==0)
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_FAR_UPDATE] native_frame=" << a.native_frame
            << " volume=" << rg_name_prefix_ << " budget=" << params_.far_update_budget
            << " total=" << params_.probe_count_x*params_.probe_count_y*params_.probe_count_z
            << " count=" << a.count << " list_hash=" << a.list_hash << " bootstrap=" << a.bootstrap
            << " epoch=" << a.epoch << " rotation=" << a.rotation << " max_age=" << a.max_age
            << " max_age_unit=recorded_updates source_ready=" << a.source_ready << " warmup=" << a.warmup
            << " uploaded=" << a.uploaded << " recorded=" << a.recorded << std::endl;
    if(rg_name_prefix_!="Far" || !lumencompare::enabled || !lumencompare::Selected("DDGI.FarState")) return;
    const u64 n=u64(params_.probe_count_x)*params_.probe_count_y*params_.probe_count_z;
    const auto capture=[&](const char* label,ResourceHandle h,u64 size) {
        if(h!=handles::INVALID_RESOURCE && size) lumencompare::BufferSlice(device_,cmd,"DDGI.FarState",label,h,0,size);
    };
    capture("sh",irradiance_buffers_[slot],n*144u); capture("depth",depth_buffers_[slot],n*512u);
    capture("confidence",confidence_buffer_,n*16u); capture("opensky",opensky_buffer_,n*8u);
    capture("relocated-origins",relocated_origins_,n*16u);
    capture("update-list",probe_update_list_buffers_[slot],u64(a.count)*4u);
    capture("volume",volume_cb_[slot],sizeof(DDGIVolumeData));
    const auto dir=std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR"))/("frame-"+std::to_string(lumencompare::ordinal));
    std::filesystem::create_directories(dir);
    std::ofstream meta(dir/"far-state.json");
    meta << "{\"native_frame\":" << a.native_frame << ",\"slot\":" << slot
        << ",\"epoch\":" << a.epoch << ",\"count\":" << a.count << ",\"budget\":" << params_.far_update_budget
        << ",\"rotation\":" << a.rotation << ",\"bootstrap\":" << a.bootstrap
        << ",\"list_hash\":" << a.list_hash << ",\"max_age\":" << a.max_age
        << ",\"source_ready\":" << a.source_ready << ",\"uploaded\":" << a.uploaded << ",\"recorded\":" << a.recorded
        << ",\"source_epoch\":" << measured_geometry_epoch_ << ",\"source_digest\":" << measured_geometry_digest_
        << ",\"age_unit\":\"successful recorded updates\",\"completion\":\"capture fence only; recorded is not GPU completion\"}\n";
}

static bool VarianceFenceEnabled(const rhi::RHIDeviceBase* device) {
    static const bool requested=[] {const char* value=std::getenv("PRIMAL_DDGI_VARIANCE_FENCED");return value && std::strcmp(value,"1")==0;}();
    return requested && device && device->GetPlatform()==rhi::RHIPlatform::Vulkan;
}
static bool VarianceFenceAuditEnabled() {
    static const bool enabled=[] {const char* v=std::getenv("DDGI_VARIANCE_FENCE_AUDIT");return v && std::strcmp(v,"1")==0;}();
    return enabled;
}
LumenDDGIPass::VarianceFenceAuditView LumenDDGIPass::GetVarianceFenceAudit() const {
    auto result=variance_audit_;result.enabled=VarianceFenceEnabled(device_);result.current_generation=variance_generation_;
    result.current_epoch=variance_scene_epoch_;result.current_digest=variance_source_digest_;return result;
}
void LumenDDGIPass::SetVarianceFenceSceneProvider(const SurfaceCachePass* provider) {
    if(!VarianceFenceEnabled(device_) || variance_scene_provider_==provider)return;
    variance_fence_ticket_.Reset();variance_cpu_confirmed_=false;variance_identity_valid_=false;
    ++variance_generation_;variance_scene_epoch_=0;variance_source_digest_=0;variance_identity_.Reset();variance_scene_provider_=provider;
    for(auto& state:probe_states_)state.max_depth_variance=0.f;
}
void LumenDDGIPass::SyncVarianceFenceScene(u64 sceneEpoch,u64 sourceDigest) {
    if(!VarianceFenceEnabled(device_))return;
    const bool previouslyConfirmed=variance_cpu_confirmed_;
    const auto action=ReconcileVarianceScene(variance_identity_,variance_fence_ticket_,variance_cpu_confirmed_,probe_states_,sceneEpoch,sourceDigest);
    if(action==VarianceIdentityAction::Invalidate || (!sceneEpoch && previouslyConfirmed))++variance_generation_;
    variance_scene_epoch_=sceneEpoch;
    variance_source_digest_=sourceDigest;
    variance_identity_valid_=variance_identity_.Available();
}
bool LumenDDGIPass::VarianceFenceSceneStillCurrent() {
    if(!VarianceFenceEnabled(device_))return false;
    if(!variance_scene_provider_) {SyncVarianceFenceScene(0,0);return false;}
    const auto witness=variance_scene_provider_->GetGeometryWitnessView(0);
    SyncVarianceFenceScene(witness.scene_epoch,witness.source_digest);
    return variance_identity_valid_ && variance_fence_ticket_.Active();
}
void LumenDDGIPass::BeginVarianceFenceFrame(u32 slot,rhi::CommandBufferHandle command,rhi::SyncHandle fence,u64 nativeFrame,u64 ticket) {
    if(VarianceFenceAuditEnabled())variance_audit_.accepted_frame=false;
    if(VarianceFenceEnabled(device_) && variance_identity_valid_) {
        auto* cmd=rhi::GetCommandBuffer(command);
        if(!cmd || cmd->GetType()!=rhi::CommandQueueType::Graphics) {variance_fence_ticket_.Reject();return;}
        variance_fence_ticket_.Begin(device_->FrameSlotCompletions(),slot,command,fence,nativeFrame,ticket);
    }
}
void LumenDDGIPass::CommitVarianceFenceFrame(u32 slot,rhi::CommandBufferHandle command,rhi::SyncHandle fence,u64 nativeFrame,u64 ticket,bool success) {
    if(VarianceFenceEnabled(device_)) {
        const bool committed=variance_fence_ticket_.Commit(slot,command,fence,nativeFrame,ticket,success);
        if(committed && VarianceFenceAuditEnabled())++variance_audit_.submitted_count;
    }
}
void LumenDDGIPass::ValidateVarianceFenceCommand(u32 slot,rhi::CommandBufferHandle command) {
    if(VarianceFenceEnabled(device_) && !variance_fence_ticket_.Matches(slot,command))variance_fence_ticket_.Reject();
}
void LumenDDGIPass::ClearProbeField() {
    ++variance_generation_;
    variance_fence_ticket_.Reset();variance_cpu_confirmed_=false;variance_identity_valid_=false;variance_scene_epoch_=0;variance_source_digest_=0;variance_identity_.Reset();
    if (!device_ || !initialized_) return;
    if(params_.deferred_field_reset) {
        field_reset_pending_=true;
        last_recorded_update_count_=0;
        if(params_.far_update_budget) far_update_policy_.Reset();
        for(auto& ps:probe_states_) ps={};
        probe_update_offset_=0; depth_ring_copies_=0; for(auto& f:depth_ring_recorded_frame_)f=~u32(0); for(auto& f:depth_ring_recorded_native_)f=~u64(0);
        volume_data_.ProbeUpdateCount=0;
        return;
    }
    const u32 probeCount = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    for (int f = 0; f < 3; f++) {
        if (depth_provenance_buffers_[f]!=handles::INVALID_RESOURCE) {
            if (auto* p=device_->MapBuffer(depth_provenance_buffers_[f])) {
                std::memset(p,0,u64(probeCount)*64u*8u*sizeof(u32));
                device_->UnmapBuffer(depth_provenance_buffers_[f]);
            }
        }
        // Irradiance: 9 vec4 coefficients per probe.  The shader/C++ buffer
        // ABI uses a 16-byte stride per coefficient; clearing only float3
        // leaves every fourth lane (and the tail of the allocation) stale.
        if (irradiance_buffers_[f] != rhi::handles::INVALID_RESOURCE) {
            if (float* irr = static_cast<float*>(device_->MapBuffer(irradiance_buffers_[f]))) {
                memset(irr, 0, static_cast<u64>(probeCount) * 9 * sizeof(math::v4));
                device_->UnmapBuffer(irradiance_buffers_[f]);
            }
        }
        // Depth history: 128 floats/probe — reset to the same "open" defaults
        // CreateProbeTextures uses (mean = ray_max_distance, var = small
        // non-zero: zero variance trips the Chebyshev singularity).
        if (depth_buffers_[f] != rhi::handles::INVALID_RESOURCE) {
            constexpr u32 kDepthTexels = 64;            // 8×8 octahedral
            constexpr u32 kFloatsPerProbe = kDepthTexels * 2;
            const float depthMeanInit = ray_sample_mode_ == 7u ? -1.0f : params_.ray_max_distance;
            const float depthVarInit = ray_sample_mode_ == 7u ? 0.0f : params_.probe_spacing * params_.probe_spacing * 0.25f;
            if (float* dep = static_cast<float*>(device_->MapBuffer(depth_buffers_[f]))) {
                for (u32 pIdx = 0; pIdx < probeCount; ++pIdx) {
                    float* probe = dep + (u64)pIdx * kFloatsPerProbe;
                    for (u32 j = 0; j < kDepthTexels; ++j) probe[j] = depthMeanInit;
                    for (u32 j = kDepthTexels; j < kFloatsPerProbe; ++j) probe[j] = depthVarInit;
                }
                device_->UnmapBuffer(depth_buffers_[f]);
            }
        }
    }

    // These single-buffered fields are part of the temporal probe state too.
    // Keeping convergence/open-sky from the pre-card phase makes a late SC
    // registration enter the settled injection path with stale visibility,
    // selecting a different fixed point than a frame-0 registration.
    if (confidence_buffer_ != rhi::handles::INVALID_RESOURCE) {
        if (float* conf = static_cast<float*>(device_->MapBuffer(confidence_buffer_))) {
            const float initAge = (static_volume_ && static_volume_->IsLoaded() && ray_sample_mode_ != 7u) ? 0.5f : 0.0f;
            for (u32 p = 0; p < probeCount; ++p) {
                conf[p * 4 + 0] = 0.0f;
                conf[p * 4 + 1] = 0.0f;
                conf[p * 4 + 2] = 1.0f;
                conf[p * 4 + 3] = initAge;
            }
            device_->UnmapBuffer(confidence_buffer_);
        }
    }
    if (opensky_buffer_ != rhi::handles::INVALID_RESOURCE) {
        if (float* sky = static_cast<float*>(device_->MapBuffer(opensky_buffer_))) {
            std::memset(sky, 0, static_cast<u64>(probeCount) * 2u * sizeof(float));
            device_->UnmapBuffer(opensky_buffer_);
        }
    }
    for (auto& ps : probe_states_) ps = {};
    probe_update_offset_ = 0;
    depth_ring_copies_ = 0; for(auto& f:depth_ring_recorded_frame_)f=~u32(0); for(auto& f:depth_ring_recorded_native_)f=~u64(0);
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[LumenDDGI] Probe field cleared — re-seeding irradiance/depth/confidence/open-sky."
        << std::endl;
}

void LumenDDGIPass::InitializeProbesFromStatic() {
    u32 probeCount = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;

    if (static_volume_ && static_volume_->IsLoaded() && ray_sample_mode_ != 7u) {
        // Validate dimensions match (Errata E9)
        if (static_volume_->GridDimX() != params_.probe_count_x ||
            static_volume_->GridDimY() != params_.probe_count_y ||
            static_volume_->GridDimZ() != params_.probe_count_z) {
            // Dimension mismatch — fall through to sky estimate
            goto sky_estimate;
        }

        // Adopt the static volume's world-space origin as the runtime probe origin.
        // Initialize() sets probe_origin_ to a params-centered position
        // (-((count-1)*spacing/2) on each axis), but the bake uses its own origin
        // (e.g. -32,-16,-32 to cover Sponza's [-32,32]³ bounds). Without this
        // sync, the GIGather shader offsets every world position by the delta
        // between the two origins, AND any pixel at world coords below the
        // runtime origin early-outs to zero (outside-grid branch). Net effect:
        // ~25% of the scene reads zero indirect, the rest reads data shifted
        // by the origin delta. Match the bake exactly to fix both.
        const math::v3& staticOrigin = static_volume_->GetParams().origin;
        probe_origin_ = staticOrigin;
        volume_data_.ProbeOrigin = math::v4{staticOrigin.x, staticOrigin.y, staticOrigin.z, 0.0f};
        PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Synced probe_origin_ to static bake: ("
                  << staticOrigin.x << ", " << staticOrigin.y << ", " << staticOrigin.z << ")" << std::endl;

        // Copy static → ALL THREE dynamic frame buffers
        const math::v3* staticIrr = static_volume_->GetIrradianceData();
        const float* staticMean = static_volume_->GetDepthMeanData();
        const float* staticVar = static_volume_->GetDepthVarData();

        for (int f = 0; f < 3; f++) {
            // Irradiance: simd::float3 has 16-byte stride (4 bytes padding).
            // DDGI buffer expects flat float[3] per coefficient. Must copy element-by-element.
            float* irrMapped = static_cast<float*>(device_->MapBuffer(irradiance_buffers_[f]));
            if (irrMapped) {
                u32 totalCoeffs = probeCount * 9;
                for (u32 i = 0; i < totalCoeffs; ++i) {
                    irrMapped[i * 3 + 0] = staticIrr[i].x;
                    irrMapped[i * 3 + 1] = staticIrr[i].y;
                    irrMapped[i * 3 + 2] = staticIrr[i].z;
                }
                device_->UnmapBuffer(irradiance_buffers_[f]);
            }

            // Depth: DDGI has 128 floats/probe (64 mean + 64 var)
            //         Static has separate arrays of 64 each
            float* depthMapped = static_cast<float*>(device_->MapBuffer(depth_buffers_[f]));
            if (depthMapped) {
                for (u32 p = 0; p < probeCount; p++) {
                    u32 ddgiBase = p * 128;
                    u32 staticBase = p * 64;
                    for (u32 oct = 0; oct < 64; oct++) {
                        depthMapped[ddgiBase + oct] = staticMean[staticBase + oct];       // mean
                        depthMapped[ddgiBase + 64 + oct] = staticVar[staticBase + oct];   // variance
                    }
                }
                device_->UnmapBuffer(depth_buffers_[f]);
            }
        }

        PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Initialized probes from static bake data ("
                  << probeCount << " probes)" << std::endl;
        return;
    }

sky_estimate:
    // No bake data or dimension mismatch: sky estimate fallback.
    // Use low-intensity neutral gray instead of blue sky color — the blue sky
    // estimate (0.5,0.7,1.0)*pi produced an unreasonable cyan glow when DDGI
    // trace fails (MoltenVK spvUnsafeArray on Vulkan). Neutral gray at low
    // intensity approximates ambient bounce without visible color cast.
    // Must use float* (not math::v3*) — buffer is 12 bytes/element, math::v3 is 16 bytes.
    math::v3 skyL0 = math::v3{0.03f, 0.03f, 0.035f}; // dim neutral ambient
    for (int f = 0; f < 3; f++) {
        // vec4 layout: 9 coeffs × 4 floats = 36 floats per probe
        u64 irrSize = (u64)probeCount * 9 * sizeof(math::v4);
        float* mapped = static_cast<float*>(device_->MapBuffer(irradiance_buffers_[f]));
        if (mapped) {
            memset(mapped, 0, irrSize);
            for (u32 p = 0; p < probeCount; p++) {
                // L0 coeff = vec4(skyL0.xyz, 0)
                mapped[p * 36 + 0] = skyL0.x;
                mapped[p * 36 + 1] = skyL0.y;
                mapped[p * 36 + 2] = skyL0.z;
            }
            device_->UnmapBuffer(irradiance_buffers_[f]);
        }
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Initialized probes with sky estimate fallback" << std::endl;
}

// ============================================================================
// Surface Cache integration
// ============================================================================

// X2/A3: 视图签名实现（旧 7 参签名在头文件内转发到此）。
// 字段映射与旧实现逐一相同——上传数据（DDGIVolumeData 的 SCLookupCount/
// SCAtlasSize）与 sc_enabled_ 判定逐字节等价。
void LumenDDGIPass::SetSurfaceCacheResources(const SurfaceCacheView& view)
{
    sc_lighting_atlas_ = view.lighting_atlas;
    sc_atlas_size_ = view.atlas_size;
    sc_lookup_count_ = view.lookup_count;

    // The pass pre-creates dummy zero/empty-grid buffers (Initialize, "Dummy
    // Surface Cache buffers" / "Dummy card-grid buffers") precisely so the
    // trace/finalize descriptor sets stay valid when SC data is absent.
    // Optional view fields (notably the T1.3 grid pair) arrive INVALID in
    // that case — keep the dummies instead of clobbering them, otherwise the
    // passes bind INVALID handles (Vulkan: VK_NULL_HANDLE descriptor writes,
    // Metal: nil buffer arguments).
    sc_card_lookup_buffer_ = view.card_lookup != handles::INVALID_RESOURCE
        ? view.card_lookup : sc_card_lookup_buffer_;
    sc_card_data_buffer_ = view.card_data != handles::INVALID_RESOURCE
        ? view.card_data : sc_card_data_buffer_;
    sc_grid_offsets_buffer_ = view.grid_offsets != handles::INVALID_RESOURCE
        ? view.grid_offsets : sc_grid_offsets_buffer_;
    sc_grid_cards_buffer_ = view.grid_cards != handles::INVALID_RESOURCE
        ? view.grid_cards : sc_grid_cards_buffer_;
    sc_depth_atlas_ = view.depth_atlas != handles::INVALID_RESOURCE
        ? view.depth_atlas : sc_depth_atlas_;   // T3.4 (D3)
    // P0-B: view 侧真实 atlas 首次接管时 prime（tracked==UNDEF 才实际提交，
    // 之后 no-op）——binding 13 以 SampledImage 采样它，承诺 SRO。
    device_->PrimeTextureLayout(sc_depth_atlas_);

    // Store SC params in DDGIVolumeData (triple-buffered, confirmed working)
    // instead of a separate sc_params buffer (binding 6 was not working).
    const bool reportCacheConfiguration = !sc_enabled_ ||
        volume_data_.SCLookupCount != view.lookup_count ||
        volume_data_.SCAtlasSize != view.atlas_size;
    volume_data_.SCLookupCount = view.lookup_count;
    volume_data_.SCAtlasSize = view.atlas_size;

    sc_enabled_ = (view.lighting_atlas != handles::INVALID_RESOURCE &&
                   view.card_lookup != handles::INVALID_RESOURCE &&
                   view.card_data != handles::INVALID_RESOURCE &&
                   view.lookup_count > 0);

    if (sc_enabled_ && reportCacheConfiguration) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Surface Cache enabled: atlas_size=" << view.atlas_size
                  << ", lookup_count=" << view.lookup_count << std::endl;
    }
}

// X2/A3: 时域策略映射 —— EMA 即现有 irradiance/depth EMA（默认，行为不变）；
// EMAVarianceClamp/Reprojection 当前无独立代码路径，暂存枚举（TODO X3）。
void LumenDDGIPass::SetTemporalStrategy(GITemporalStrategy strategy) {
    if (temporal_strategy_ == strategy) return;
    temporal_strategy_ = strategy;
    PRIMAL_LOG_STREAM(Info, "Render") << "[LumenDDGI] Temporal strategy -> "
              << GITemporalStrategyName(strategy)
              << " (EMA path unchanged; full mapping lands in X3)" << std::endl;
}

void LumenDDGIPass::ClearSurfaceCacheResources() {
    sc_enabled_ = false;
    // Do NOT clear buffer/atlas handles — Metal validates all declared
    // bindings must be valid, even when the shader skips the SC loop.
    // Setting SCLookupCount=0 ensures the finalize pass never reads them.
    sc_atlas_size_ = 0;
    sc_lookup_count_ = 0;
    volume_data_.SCLookupCount = 0;
    volume_data_.SCAtlasSize = 0;
}

// ============================================================================
// AddPass -- main entry point called per frame
// ============================================================================

bool LumenDDGIPass::UpdateProbeOrigin(const math::v3& camera_position)
{
    if (!initialized_) return false;

    // Static scene: lock probe origin at initial center position (set in Initialize).
    // Do not follow camera — prevents SDF/probe mismatch and multi-bounce instability.
    //
    // DO NOT delete this early return to re-enable camera scrolling. The code
    // below is a dormant skeleton; scrolling is UNSAFE until all of:
    //   1. probe_positions_ — filled ONCE at Initialize with absolute world
    //      positions; a shifted grid makes every entry stale.
    //   2. ProbeRelocationShift — declared in every DDGI UBO but READ BY NO
    //      SHADER; nothing consumes a nonzero shift today.
    //   3. History relocation — irradiance/depth history is indexed by
    //      physical probe slot with no ring offset or shift-aware fetch, so a
    //      scrolled grid would bind old radiance to new world positions.
    // Correct design: logical coord + ring offset for history, positions
    // derived as volumeOrigin + logicalCoord * spacing (never persisted).
    return false;

    float spacing = params_.probe_spacing;
    // Snap camera to grid: only shifts when camera crosses a spacing boundary
    math::v3 snappedCam{
        std::floor(camera_position.x / spacing) * spacing,
        std::floor(camera_position.y / spacing) * spacing,
        std::floor(camera_position.z / spacing) * spacing
    };

    // Center the grid around the snapped camera position
    math::v3 halfGrid{
        float(params_.probe_count_x) * 0.5f * spacing,
        float(params_.probe_count_y) * 0.5f * spacing,
        float(params_.probe_count_z) * 0.5f * spacing
    };
    math::v3 newOrigin = snappedCam - halfGrid;

    bool shifted = false;
    if (std::abs(newOrigin.x - probe_origin_.x) > 0.01f ||
        std::abs(newOrigin.y - probe_origin_.y) > 0.01f ||
        std::abs(newOrigin.z - probe_origin_.z) > 0.01f)
    {
        // Compute shift in probe cells
        float invSpacing = 1.0f / spacing;
        relocation_shift_[0] = (int)std::round((newOrigin.x - probe_origin_.x) * invSpacing);
        relocation_shift_[1] = (int)std::round((newOrigin.y - probe_origin_.y) * invSpacing);
        relocation_shift_[2] = (int)std::round((newOrigin.z - probe_origin_.z) * invSpacing);

        probe_origin_ = newOrigin;
        volume_data_.ProbeOrigin = {newOrigin.x, newOrigin.y, newOrigin.z, 0.0f};
        last_snapped_cam_ = snappedCam;
        shifted = true;
    } else {
        // No shift this frame — reset relocation
        relocation_shift_[0] = 0;
        relocation_shift_[1] = 0;
        relocation_shift_[2] = 0;
    }

    return shifted;
}

// ============================================================================

LumenDDGIOutput LumenDDGIPass::AddPass(
    RenderGraph& graph,
    RGResourceHandle prev_frame_color,
    const DDGICameraData& camera_data,
    u32 current_frame_index,
    RGResourceHandle sc_lighting_atlas)
{
    LumenDDGIOutput output{};
    last_recorded_update_count_=0;

    // Phase 1d: 槽位单一事实源 DDGISlotMap.h（writeIndex = currentIndex =
    // 本帧写槽；previousIndex = 上一完成帧，语义同原 histIdx）。
    const auto slots = DDGISlotsForFrame(current_frame_index);
    const u32 outIdx = slots.writeIndex;
    const u32 histIdx = slots.previousIndex;
    exec_frame_index_ = current_frame_index;   // §8.3 CPU 读回的帧门用

    // Import persistent probe buffers/textures into render graph.
    // T4.4: rg_name_prefix_ disambiguates near/far volume instances.
    const std::string tag = rg_name_prefix_.empty() ? std::string{} : (rg_name_prefix_ + "_");
    auto irradianceOutHandle = graph.ImportResource(
        tag + "LumenDDGI_Irradiance_" + std::to_string(outIdx), irradiance_buffers_[outIdx]);
    auto irradianceHistHandle = graph.ImportResource(
        tag + "LumenDDGI_IrradianceHist_" + std::to_string(histIdx), irradiance_buffers_[histIdx]);
    auto depthOutHandle = graph.ImportResource(
        tag + "LumenDDGI_Depth_" + std::to_string(outIdx), depth_buffers_[outIdx]);
    auto depthHistHandle = graph.ImportResource(
        tag + "LumenDDGI_DepthHist_" + std::to_string(histIdx), depth_buffers_[histIdx]);
    RGResourceHandle provenanceOutHandle{},provenanceHistHandle{};
    if (depth_provenance_enabled_) {
        provenanceOutHandle=graph.ImportResource(tag+"DDGI_DepthProvenance_"+std::to_string(outIdx),depth_provenance_buffers_[outIdx]);
        provenanceHistHandle=graph.ImportResource(tag+"DDGI_DepthProvenanceHist_"+std::to_string(histIdx),depth_provenance_buffers_[histIdx]);
    }

    output.ddgi_irradiance = irradianceOutHandle;
    output.ddgi_irradiance_hist = irradianceHistHandle;

    graph.AddPass<LumenDDGIData>(rg_name_prefix_.empty() ? "LumenDDGI" : ("LumenDDGI_" + rg_name_prefix_),
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, irradianceOutHandle, irradianceHistHandle,
         depthOutHandle, depthHistHandle, provenanceOutHandle, provenanceHistHandle, sc_lighting_atlas, camera_data](
            LumenDDGIData& data, RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling — outputs go to imported buffers
            // Read previous frame color (for lighting lookup during trace)
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;
            if (camera_data.screen_depth.IsValid())
                builder.Read(camera_data.screen_depth, ResourceState::ShaderResource);
            if (camera_data.radiance_depth.IsValid())
                builder.Read(camera_data.radiance_depth, ResourceState::ShaderResource);

            // Declare dependency on SC lighting atlas when SC is active.
            // Without this, the RG has no barrier between SC Lighting write
            // and DDGI finalize read — data race on the atlas texture.
            if (sc_lighting_atlas.IsValid()) {
                builder.Read(sc_lighting_atlas, ResourceState::ShaderResource);
            }

            // Write to output irradiance/depth textures
            builder.Write(irradianceOutHandle, ResourceState::UnorderedAccess);
            builder.Read(irradianceHistHandle, ResourceState::ShaderResource);
            builder.Write(depthOutHandle, ResourceState::UnorderedAccess);
            builder.Read(depthHistHandle, ResourceState::ShaderResource);
            if (provenanceOutHandle.IsValid()) {
                builder.Write(provenanceOutHandle,ResourceState::UnorderedAccess);
                builder.Read(provenanceHistHandle,ResourceState::ShaderResource);
            }
        },

        // ====================================================================
        // Execute lambda: dispatch 3 compute sub-passes
        // ====================================================================
        [this, camera_data, current_frame_index, outIdx, histIdx,
         prev_frame_color, irradianceOutHandle, irradianceHistHandle,
         depthOutHandle, depthHistHandle](
            const LumenDDGIData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            const bool farPolicy=params_.far_update_budget!=0;
            if(params_.deferred_field_reset) RecordFieldReset(cmd);
            if(farPolicy) {
                // Also preserve history when warmup, missing sources or a failed
                // upload prevents dispatch. A slot number is not a completion proof.
                InheritProbeHistory(cmd,outIdx,histIdx);
                far_update_audit_={}; far_update_audit_.native_frame=current_frame_index;
                far_update_audit_.epoch=far_update_policy_.Epoch();
                far_update_audit_.rotation=far_update_policy_.Rotation();
                far_update_audit_.bootstrap=far_update_policy_.BootstrapPending();
                far_update_audit_.max_age=far_update_policy_.MaxAge();
                far_update_audit_.warmup=camera_data.gi_frame_index<GIWarmupFrames();
                volume_data_.ProbeUpdateCount=0;
            }
            struct FarAuditScope {
                LumenDDGIPass* pass; RHICommandBuffer* cmd; u32 slot; bool enabled;
                ~FarAuditScope() {if(enabled) pass->PublishFarUpdateAudit(cmd,slot);}
            } farAuditScope{this,cmd,outIdx,farPolicy};

            // T4.5: unified debug knobs — freeze frame / per-pass toggle.
            // Skipping dispatches keeps the triple-buffered probe data at its
            // last-written content (readers still get valid slots).
            if (GIDebug().freeze_probe_updates || !GIDebug().ddgi_trace) return;

            static int s_ddgi_exec = 0;
            if (s_ddgi_exec < 3) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_EXEC] frame " << s_ddgi_exec
                          << " dynamic=" << dynamic_mode_
                          << " initialized=" << initialized_ << std::endl;
                s_ddgi_exec++;
            }

            // === [Mode11 Perf] per-section timing instrumentation ===
            // Tracks CPU-side encoding/upload time within the Execute lambda.
            // GPU work is async — Dispatch returns immediately. If GPU were the
            // bottleneck, these CPU times would all be <5ms; GPUTask in the trace
            // would show the actual GPU cost.
            namespace chr = std::chrono;
            struct PerfAccum {
                u64 total_us = 0, upload_global_us = 0, upload_volume_us = 0;
                u64 trace_us = 0, irradiance_us = 0, depth_us = 0;
                u32 samples = 0;
            };
            static PerfAccum s_perf;
            auto T0 = chr::steady_clock::now();
            auto _sec = [](chr::steady_clock::time_point a, chr::steady_clock::time_point b) -> u64 {
                return (u64)chr::duration_cast<chr::microseconds>(b - a).count();
            };
            chr::steady_clock::time_point _t1, _t2, _t3, _t4, _t5;

            // Static-bake mode (Mode 10): skip runtime TraceRays/UpdateIrradiance/UpdateDepth.
            // The static data was copied into all 3 irradiance_buffers_/depth_buffers_
            // by InitializeProbesFromStatic; runtime EMA blending would progressively
            // decay that data toward runtime-traced radiance (which double-counts
            // direct light via prevHdrTexture_). The RG still gets valid Write
            // declarations from the setup lambda above, so downstream GIGather's
            // Read dependency resolves correctly.
            //
            // Mode 11 (MeshletDynamicDDGI): dynamic_mode_==true flips this off so the
            // canonical runtime path runs every frame. Static data still seeds frame 0
            // (InitializeProbesFromStatic copied it into all 3 irradiance_buffers_),
            // then EMA blends toward canonical dynamic DDGI.
            if (static_volume_ && static_volume_->IsLoaded() && !dynamic_mode_ && ray_sample_mode_ != 7u) {
                return;
            }

            u32 frameIdx = DDGISlotsForFrame(current_frame_index).writeIndex;

            u32 probeCountTotal = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
            u32 updateCount = std::min(max_probes_per_frame_, probeCountTotal);
            // C3.5 slice 3 (deterministic stabilization): during the warm-up
            // window the probe field stays frozen at its deterministic init
            // while the capture and direct lighting converge the atlas —
            // updating probes against the transient is what selects the
            // multi-stable branch (09-10 十二). Single choke point: zeroing
            // updateCount no-ops every downstream update dispatch (zero-size
            // guards from C0 cover the dispatches). Phase 1a: knob shared
            // with SC via GIWarmup.h — unified default 90 (was 0 here, which
            // left the two modules' warmup windows inconsistent without env;
            // see remediation plan §执行日志 for the recorded rationale).
            {
                const u32 kWarmupFrames = GIWarmupFrames();
                if (kWarmupFrames > 0u && camera_data.gi_frame_index < kWarmupFrames) updateCount = 0;
            }

            // Resolve physical handles from render graph
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                if (!handle.IsValid()) return handles::INVALID_RESOURCE;
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle prevColorTex = ResolveTexture(prev_frame_color);
            const auto screenDepthTex = ResolveTexture(camera_data.screen_depth);
            const auto radianceDepthTex = ResolveTexture(camera_data.radiance_depth);

            // Resolve SDF cascade textures from GlobalSDF, or use offline SDF if set.
            ResourceHandle sdfTextures[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
            };
            bool sdfAvailable = false;

            if (has_offline_sdf_) {
                // Offline SDF mode: single high-res volume replaces all cascades.
                sdfTextures[0] = offline_sdf_texture_;
                sdfTextures[1] = offline_sdf_texture_; // alias to keep descriptor valid
                sdfTextures[2] = offline_sdf_texture_;
                sdfAvailable = (sdfTextures[0] != handles::INVALID_RESOURCE);
            } else {
                auto& sdf = nanite::GlobalSDF::Get();
                sdfAvailable = sdf.IsInitialized();
                if (sdfAvailable) {
                    for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                        const auto& cascade = sdf.GetCascade(c);
                        if (cascade.sdf_texture != handles::INVALID_RESOURCE)
                            sdfTextures[c] = cascade.sdf_texture;
                    }
                    sdfAvailable = (sdfTextures[0] != handles::INVALID_RESOURCE);
                }
            }

            FarUpdatePolicy::Plan farPlan;
            if(farPolicy) {
                bool ready=camera_data.gi_content_ready && sdfAvailable && dynamic_mode_ &&
                    sdf_trace_pipeline_!=handles::INVALID_PIPELINE && finalize_pipeline_!=handles::INVALID_PIPELINE &&
                    irradiance_pipeline_!=handles::INVALID_PIPELINE && depth_pipeline_!=handles::INVALID_PIPELINE &&
                    hit_distance_buffer_!=handles::INVALID_RESOURCE && ray_data_buffer_!=handles::INVALID_RESOURCE &&
                    probe_update_list_buffers_[frameIdx]!=handles::INVALID_RESOURCE &&
                    irradiance_buffers_[outIdx]!=handles::INVALID_RESOURCE && irradiance_buffers_[histIdx]!=handles::INVALID_RESOURCE &&
                    depth_buffers_[outIdx]!=handles::INVALID_RESOURCE && depth_buffers_[histIdx]!=handles::INVALID_RESOURCE &&
                    global_cb_[frameIdx]!=handles::INVALID_RESOURCE && volume_cb_[frameIdx]!=handles::INVALID_RESOURCE &&
                    finalize_vp_cb_[frameIdx]!=handles::INVALID_RESOURCE && detail_table_cb_[frameIdx]!=handles::INVALID_RESOURCE &&
                    confidence_buffer_!=handles::INVALID_RESOURCE && opensky_buffer_!=handles::INVALID_RESOURCE &&
                    relocated_origins_!=handles::INVALID_RESOURCE &&
                    sdf_trace_ds_[frameIdx]!=handles::INVALID_DESCRIPTOR_SET && finalize_ds_[frameIdx]!=handles::INVALID_DESCRIPTOR_SET &&
                    irradiance_ds_[frameIdx]!=handles::INVALID_DESCRIPTOR_SET && depth_ds_[frameIdx]!=handles::INVALID_DESCRIPTOR_SET;
                if(measured_geometry_requested_) {
                    ready &= measured_geometry_epoch_!=0 && measured_geometry_digest_!=0;
                    for(auto h:measured_geometry_) ready &= h!=handles::INVALID_RESOURCE;
                }
                far_update_audit_.source_ready=ready;
                farPlan=far_update_policy_.Select(ready,far_update_audit_.warmup);
                updateCount=static_cast<u32>(farPlan.indices.size());
                if(!updateCount) return;
            }

            // Ensure SDF cascade textures are in SHADER_READ_ONLY layout for trace.
            // DispatchVoxelization (in RenderWithCommandBuffer, before this RG)
            // leaves them in SHADER_READ_ONLY after its UAV→SRV barrier, but
            // the layout tracking may be stale. Force transition to be safe.
            if (sdfAvailable) {
                for (u32 c = 0; c < 3u; ++c) {
                    if (sdfTextures[c] != handles::INVALID_RESOURCE) {
                        ResourceBarrier sdfBarrier{};
                        sdfBarrier.resource = sdfTextures[c];
                        sdfBarrier.beforeState = ResourceState::ShaderResource;
                        sdfBarrier.afterState = ResourceState::ShaderResource;
                        sdfBarrier.subresource = 0xFFFFFFFF;
                        sdfBarrier.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&sdfBarrier, 1);
                    }
                }
            }

            // Mode 11 per-vertex albedo: GBuffer albedo from GPUDrivenDrawPipeline.
            // Colored surfaces (fabric, painted walls) bounce colored light,
            // visibly distinguishing Mode 11 from Mode 10's uniform 0.5 albedo.
            // Falls back to INVALID_RESOURCE in non-meshlet modes (sampleHitAlbedo
            // WGSL falls back to volume.Albedo via the textureLoad result being
            // undefined—but the binding still must be valid for the layout to
            // pass validation). Use prevColorTex as a safe non-null placeholder
            // when GBuffer isn't available.
            ResourceHandle gbufferAlbedoTex = prevColorTex;
            if (dynamic_mode_) {
                gbufferAlbedoTex = nanite::GPUDrivenDrawPipeline::Get().GetGBufferAlbedo();
                if (gbufferAlbedoTex == handles::INVALID_RESOURCE) {
                    gbufferAlbedoTex = prevColorTex;
                }
            }

            // Mode 11 safety net: canonical trace requires a populated GlobalSDF.
            // If the host (e.g. TestDawnForwardRenderer) hasn't initialized /
            // voxelized GlobalSDF, trace writes nothing and ray_data_buffer_
            // stays zero-initialized. UpdateIrradiance would then EMA-blend the
            // static seed toward zero — visibly decaying Mode 11 to "no DDGI
            // effect" over ~60 frames. Skip the entire runtime path instead;
            // the static seed copied by InitializeProbesFromStatic stays put.
            // Setup lambda's Write declarations still satisfy downstream
            // GIGather's Read dependency (same pattern as the Mode 10 path).
            if (dynamic_mode_ && !sdfAvailable) {
                static bool warned = false;
                if (!warned) {
                    PRIMAL_LOG_STREAM(Error, "Render") << "[Mode11] GlobalSDF unavailable — falling back to static seed\n";
                    warned = true;
                }
                return;
            }
            if (dynamic_mode_) {
                static bool traced = false;
                if (!traced) {
                    PRIMAL_LOG_STREAM(Error, "Render") << "[Mode11] GlobalSDF available — runtime trace active\n";
                    traced = true;
                }
            }

            // === [Mode 11 Diagnostic] One-shot dump of dispatch-relevant state ===
            if (dynamic_mode_) {
                static bool diagOnce = false;
                if (!diagOnce) {
                    diagOnce = true;
                    std::fprintf(stderr,
                        "=== [Mode11 Diag] frame=%u ===\n"
                        "  sdfAvailable=%d sdfTex[0..2]=%llu %llu %llu\n"
                        "  trace_pipeline=%llu sdf_trace_pipeline=%llu finalize_pipeline=%llu\n"
                        "  ray_data_buffer=%llu hit_distance_buffer=%llu\n"
                        "  prevColorTex=%llu gbufferAlbedoTex=%llu\n"
                        "  sc_enabled=%d updateCount=%u rays/probe=%u\n"
                        "  probeCountTotal=%u max_probes_per_frame=%u\n",
                        (unsigned)current_frame_index,
                        sdfAvailable ? 1 : 0,
                        (unsigned long long)sdfTextures[0],
                        (unsigned long long)sdfTextures[1],
                        (unsigned long long)sdfTextures[2],
                        (unsigned long long)trace_pipeline_,
                        (unsigned long long)sdf_trace_pipeline_,
                        (unsigned long long)finalize_pipeline_,
                        (unsigned long long)ray_data_buffer_,
                        (unsigned long long)hit_distance_buffer_,
                        (unsigned long long)prevColorTex,
                        (unsigned long long)gbufferAlbedoTex,
                        sc_enabled_ ? 1 : 0,
                        (unsigned)updateCount,
                        (unsigned)params_.rays_per_probe,
                        (unsigned)probeCountTotal,
                        (unsigned)max_probes_per_frame_);
                    std::fflush(stderr);
                }
            }

            _t1 = chr::steady_clock::now();
            // ---- Upload GlobalShaderData ----
            {
                // Must match CommonTypes.metal GlobalShaderData layout
                struct GlobalShaderData {
                    math::m4x4 View;
                    math::m4x4 Projection;
                    math::m4x4 InvProjection;
                    math::m4x4 ViewProjection;
                    math::m4x4 PreviousViewProjection;
                    math::m4x4 InvViewProjection;
                    math::v4   CameraPositionAndViewWidth;
                    math::v4   CameraDirectionAndViewHeight;
                    u32        NumDirectionalLights;
                    float      DeltaTime;
                    float      FrameCount;
                    float      padding;
                };

                auto* mapped = static_cast<GlobalShaderData*>(device_->MapBuffer(global_cb_[frameIdx]));
                if(farPolicy && !mapped) return;
                if (mapped) {
                    mapped->View = camera_data.view_matrix;
                    mapped->Projection = camera_data.proj_matrix;
                    mapped->InvProjection = rhi::math::Inverse(camera_data.proj_matrix);
                    mapped->ViewProjection = camera_data.proj_matrix * camera_data.view_matrix;
                    mapped->PreviousViewProjection = camera_data.prev_proj_matrix * camera_data.prev_view_matrix;
                    mapped->InvViewProjection = rhi::math::Inverse(mapped->ViewProjection);

                    // Cache for finalize's screen-space prev_color sampling.
                    current_view_projection_ = mapped->ViewProjection;

                    math::m4x4 invView = rhi::math::Inverse(camera_data.view_matrix);
                    mapped->CameraPositionAndViewWidth = {
                        math::elem(invView, 3, 0), math::elem(invView, 3, 1),
                        math::elem(invView, 3, 2), (float)0.0f  // No screen dimensions needed for DDGI
                    };

                    auto& vm = camera_data.view_matrix;
                    mapped->CameraDirectionAndViewHeight = {
                        -math::elem(vm, 2, 0), -math::elem(vm, 2, 1),
                        -math::elem(vm, 2, 2), 0.0f
                    };

                    mapped->NumDirectionalLights = 0;
                    mapped->DeltaTime = camera_data.delta_time;
                    mapped->FrameCount = (float)camera_data.frame_index;
                    device_->UnmapBuffer(global_cb_[frameIdx]);
                }
            }

            _t2 = chr::steady_clock::now();
            bool volumeUploaded=false;
            // ---- Upload DDGIVolumeData ----
            {
                auto* mapped = static_cast<DDGIVolumeData*>(device_->MapBuffer(volume_cb_[frameIdx]));
                if (mapped) {
                    DDGIVolumeData vd{};
                    // v4 to match Metal float3 16-byte alignment
                    vd.ProbeOrigin = {probe_origin_.x, probe_origin_.y, probe_origin_.z, 0.0f};
                    vd.ProbeSpacing = params_.probe_spacing;
                    // _pad_before_counts[3] auto-zeroed by {}
                    vd.ProbeCounts[0] = params_.probe_count_x;
                    vd.ProbeCounts[1] = params_.probe_count_y;
                    vd.ProbeCounts[2] = params_.probe_count_z;
                    // GI 根治 Phase 4: 复用 padding 槽传有符号场旗标
                    //（零穿越命中判定；无布局变化，Metal/comp 声明同步）。
                    vd.ProbeCounts[3] = coarse_sdf_signed_ ? 1u : 0u;
                    vd.RaysPerProbe = params_.rays_per_probe;
                    vd.ProbeCountTotal = probeCountTotal;
                    vd.IrradianceBlurSigma = params_.irradiance_temporal_weight;
                    vd.DepthBlurSigma = params_.depth_temporal_weight;
                    vd.DeltaTime = camera_data.delta_time;
                    vd.FrameIndex = camera_data.frame_index;
                    vd.RayMaxDistance = params_.ray_max_distance;
                    // T4.5: unified three-backend debug knobs (GIDebug()).
                    // DawnDebugParams sliders bridge in via ApplyLegacy.
                    vd.ProbeHysteresis = GIDebug().probe_hysteresis;
                    // 2026-09-10: PRIMAL_DDGI_TEMPORAL_ALPHA overrides the
                    // settled EMA alpha. Diagnosis (multi-bounce专项): with
                    // probes updating once per ~64 frames (governor age
                    // floor), alpha 0.02 gives an effective time constant of
                    // ~3200 frames — ground probes locked into the dark
                    // branch during the atlas warm-up never re-sample evidence
                    // (multi-bounce transport starves even with a bright SC
                    // atlas). A/B knob; default keeps the configured value.
                    vd.TemporalAlpha = params_.irradiance_temporal_weight; // EMA alpha (default 0.05)
                    if (const char* e = std::getenv("PRIMAL_DDGI_TEMPORAL_ALPHA")) {
                        const float v = std::atof(e);
                        if (v > 0.0f && v <= 1.0f) vd.TemporalAlpha = v;
                    }
                    // 2026-09-11: hold the young-probe alpha (0.35) until this
                    // frame — probes keep tracking fresh evidence instead of
                    // drifting to their equilibrium over minutes (the "indirect
                    // fading after entry" perception). Fixed point unchanged.
                    if (const char* e = std::getenv("PRIMAL_DDGI_ALPHA_HOLD")) {
                        if (current_frame_index < (u32)std::max(0, std::atoi(e))) vd.TemporalAlpha = 0.35f;
                    }
                    // GI 能量所有权修复 Phase 5a: 事件驱动窗（灯光修改 →
                    // OnLightingChanged）——窗内探针保持年轻档 α=0.35 加速
                    // 重收敛，与 env 版同一机制、由事件而非常量帧数驱动。
                    if (current_frame_index < alpha_hold_until_frame_) vd.TemporalAlpha = 0.35f;
                    vd.LightDirection = {camera_data.light_direction.x,
                                         camera_data.light_direction.y,
                                         camera_data.light_direction.z, 0.0f};
                    // Mode 11 canonical: .w = light intensity (matches
                    // ProbeBakingScene::light_intensity = 3.0 from
                    // TestDawnForwardRenderer.cpp:2407).
                    vd.LightColor = {camera_data.light_color.x,
                                     camera_data.light_color.y,
                                     camera_data.light_color.z, 3.0f};
                    // M2: light-response half-life gate — override AFTER the
                    // per-frame fill so the probe trace sees the stepped
                    // light without a (field-resetting) pass rebuild.
                    if (probe_light_override_active_) {
                        vd.LightColor = probe_light_override_;
                    }
                    // Mode 11 canonical radiance inputs — live-tunable (default
                    // sky 0.4 / albedo 0.5). SkyColor.xyz preserve the 0.85/0.85/1.0
                    // tint ratio; Albedo.xyz are set uniformly.
                    float sky = GIDebug().sky_color_intensity;
                    float alb = GIDebug().albedo_intensity;
                    // T8.13 P4: fully neutral sky tint — the old
                    // (0.85,0.85,1.0) blue bias accumulated in every probe
                    // via miss rays (measured B-R = +0.03..0.06 across all
                    // gather windows), washing out colored bounce. The blue
                    // cast belongs to the sky VISUAL, not the ambient
                    // integration constant.
                    // GI 根治 Phase 3.3: 远场 volume 天空 ×0.5 档——远场
                    // 只表达低频环境，逃逸比例天然高，全额天空会让远场探针
                    // 成为封闭区的隐性泵浦（分类加权的供给端补充）。
                    vd.SkyColor = {sky * far_sky_scale_, sky * far_sky_scale_,
                                   sky * far_sky_scale_, static_cast<float>(ResolveDDGIAnalyticHistoryMode(
                                       std::getenv("DDGI_ANALYTIC_NO_HISTORY"),
                                       std::getenv("DDGI_ANALYTIC_LEGACY_RADIANCE"),
                                       std::getenv("DDGI_ANALYTIC_IRRADIANCE")))};
                    // Albedo.w is an unused DDGI VolumeData component across
                    // all current consumers (Albedo.xyz remains the material
                    // fallback).  Vulkan split shaders use it only as the
                    // latched source-audit flag; other backends always see 0.
                    vd.Albedo   = {alb, alb, alb, ray_source_audit_enabled_ ? 1.0f : 0.0f};

                    // Fill SDF cascade data — offline SDF or runtime GlobalSDF.
                    if (has_offline_sdf_) {
                        static bool sdfCbLogged = false;
                        if (!sdfCbLogged) {
                            sdfCbLogged = true;
                            PRIMAL_LOG_STREAM(Info, "Render")
                                << "[DDGI_SDF_CB] origin=(" << offline_sdf_origin_.x << ","
                                << offline_sdf_origin_.y << "," << offline_sdf_origin_.z
                                << ") extent=(" << offline_sdf_extent_.x << ","
                                << offline_sdf_extent_.y << "," << offline_sdf_extent_.z
                                << ") res=" << offline_sdf_resolution_
                                << " rayMax=" << volume_data_.RayMaxDistance << std::endl;
                        }
                        vd.SdfOrigins[0] = {offline_sdf_origin_.x, offline_sdf_origin_.y,
                                            offline_sdf_origin_.z, 0.0f};
                        vd.SdfExtents[0] = {offline_sdf_extent_.x, offline_sdf_extent_.y,
                                            offline_sdf_extent_.z, 0.0f};
                        float maxExt = std::max({offline_sdf_extent_.x, offline_sdf_extent_.y,
                                                 offline_sdf_extent_.z});
                        vd.SdfVoxelSizes[0] = {maxExt / (float)offline_sdf_resolution_, 0, 0, 0};
                        if (device_->GetPlatform()==rhi::RHIPlatform::Vulkan)
                            vd.SdfVoxelSizes[0].y=float(offline_hit_evidence_);
                        // Diagnostic dual policy: keep scalar radiance/relocation
                        // rules while only mode5 extras use measured G evidence.
                        // RG cache R must match the incumbent scalar cache (audit
                        // preflight); this flag is NOT a geometry-validity mark.
                        if (device_->GetPlatform()==rhi::RHIPlatform::Vulkan &&
                            offline_hit_evidence_>0 && ray_sample_mode_==5 &&
                            std::getenv("DDGI_DEPTH_ONLY_EVIDENCE")) {
                            vd.SdfVoxelSizes[0].y=0.f;
                            vd.SdfVoxelSizes[1].w=float(offline_hit_evidence_);
                        }
                        // Evidence G is a measured distance, unlike scalar R's
                        // conservative stepping bounds. Never enable on R-only.
                        if (device_->GetPlatform()==rhi::RHIPlatform::Vulkan &&
                            offline_hit_evidence_>0 && std::getenv("DDGI_EVIDENCE_NORMAL"))
                            vd.SdfVoxelSizes[0].z=1.f;
                        vd.SdfResolutions[0] = offline_sdf_resolution_;
                        vd.SdfCascadeCount = 1;
                    } else {
                        auto& sdf = nanite::GlobalSDF::Get();
                        for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                            const auto& cascade = sdf.GetCascade(c);
                            vd.SdfOrigins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                            vd.SdfVoxelSizes[c] = {cascade.voxel_size, 0.0f, 0.0f, 0.0f};
                            vd.SdfExtents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                            vd.SdfResolutions[c] = cascade.resolution;
                        }
                        vd.SdfCascadeCount = sdf.GetConfig().cascade_count;
                    }

                    if (measured_geometry_requested_) {
                        bool ready=true;
                        for (const auto buffer:measured_geometry_) ready &= buffer!=handles::INVALID_RESOURCE;
                        vd.SdfVoxelSizes[2].w=ready ? 1.0f : 0.0f;
                        vd.SdfVoxelSizes[2].z=ready && measured_owner_directory_ready_ && sc_lookup_count_>0u ? 1.0f : 0.0f;
                    }

                    vd.ProbeUpdateCount = updateCount;  // matches actual dispatch count
                    vd.ProbeRelocationShift[0] = relocation_shift_[0];
                    vd.ProbeRelocationShift[1] = relocation_shift_[1];
                    vd.ProbeRelocationShift[2] = relocation_shift_[2];
                    vd.RaySampleMode = (int)ray_sample_mode_;
                    // Mode 5 needs split-SDF's appended depth-only distances.
                    // Never consume uninitialized extras on the unified or
                    // no-SDF fallback path; its radiance sampler is mode 0.
                    if (vd.RaySampleMode == 5 &&
                        (!sdfAvailable || sdf_trace_pipeline_ == handles::INVALID_PIPELINE ||
                         finalize_pipeline_ == handles::INVALID_PIPELINE ||
                         hit_distance_buffer_ == handles::INVALID_RESOURCE))
                        vd.RaySampleMode = 0;

                    // Preserve Surface Cache params (set by SetSurfaceCacheResources)
                    // These are lost when vd{} zero-initializes. Must copy from members.
                    vd.SCLookupCount = sc_lookup_count_;
                    vd.SCAtlasSize = sc_atlas_size_;

                    // Opt-in energy ablation: split-finalize analytic HIT
                    // fallback drops only history bounce, not SC/screen/sky.
                    if (device_->GetPlatform()==rhi::RHIPlatform::Vulkan) {
                        // Opt-in split-finalize environment boundary ablation.
                        // Spare SdfOrigins[0].w stores suppression (default0).
                        // Does not alter visible sky, SC or analytic HIT Lo.
                        if (const char* e=std::getenv("DDGI_LOWER_ENV_SCALE")) {
                            char* end=nullptr;
                            const float scale=std::strtof(e,&end);
                            if (end!=e && *end=='\0' && std::isfinite(scale)) {
                                vd.SdfOrigins[0].w=1.f-std::clamp(scale,0.f,1.f);
                                static bool lowerEnvLogged=false;
                                if (!lowerEnvLogged) {
                                    lowerEnvLogged=true;
                                    PRIMAL_LOG_STREAM(Info,"Render")
                                        << "[DDGI_LOWER_ENV] scale=" << 1.f-vd.SdfOrigins[0].w
                                        << " split-finalize only" << std::endl;
                                }
                            }
                        }
                        // Matches the appended capacity allocated at init.
                        if (analytic_audit_enabled_) vd.SdfVoxelSizes[0].w=1.f;
                        if (material_projection_enabled_ && offline_hit_evidence_>0)
                            vd.SdfExtents[0].w=1.f;
                        // Candidate-only control word.  Scalar mode 0 is a
                        // prerequisite, so this spare lane cannot collide
                        // with the mode-5 depth-only evidence diagnostic.
                        // 1 = rho, 2 = control, 3 = rho+diagnostic, 4 =
                        // control+diagnostic, 5 = position, 6 = position+diagnostic,
                        // 7 = SC-path first native material/alpha source audit only.
                        // Diagnostic writes are latched
                        // only for DDGI_DUMP=2 plus source-audit prerequisites.
                        if (analytic_first_rho_requested_) {
                            const u32 mode = analytic_sc_first_audit_ ? 7u :
                                analytic_first_position_apply_
                                    ? (analytic_first_rho_audit_ ? 6u : 5u)
                                    : ((analytic_first_rho_apply_ ? 1u : 2u) +
                                       (analytic_first_rho_audit_ ? 2u : 0u));
                            vd.SdfExtents[1].w = static_cast<float>(mode);
                        }
                        // SdfExtents[2].w is an otherwise unused Vulkan DDGI
                        // volume slot.  Set it only after all SDF cascade
                        // fields and existing diagnostic slots are prepared;
                        // the finalize shader uses it solely to publish the
                        // optional query-source audit tail.
                        vd.SdfExtents[2].w = query_source_audit_enabled_ ? 1.f : 0.f;
                    }
                    *mapped = vd;
                    device_->UnmapBuffer(volume_cb_[frameIdx]);

                    // Store for C++ access via GetVolumeData()
                    volume_data_ = vd;
                    volumeUploaded=true;
                }
            }

            // ================================================================
            // Probe update scheduling — priority-based partial updates
            // ================================================================
            // updateCount already initialized above (line 962) as min(max_probes_per_frame_, probeCountTotal).
            // First frame: full update of all probes to initialize irradiance/depth.
            // Subsequent frames: priority scheduling updates only top-N probes.
            bool isFirstFrame = (camera_data.frame_index <= 1);

            // Read back variance from previous frame's depth buffer for priority scheduling.
            // histIdx is 2 frames behind — GPU has long since finished writing to it.
            //
            // SKIP ON WASM/Dawn: MapBuffer on a GPU-only storage buffer returns a
            // zeroed staging buffer (per Dawn backend semantics), and the Unmap
            // writes those zeros back into the GPU buffer — clobbering the depth
            // history that next frame's temporal EMA depends on. The readback is
            // also a GPU sync point, killing CPU/GPU parallelism and tanking
            // framerate (Mode 11 dropped to ~2 FPS with it on). Fall back to
            // distance + age priority only on WASM.
            // T1.3 (C5): the variance source is the readback ring copy from
            // two frames ago — identical data to what the old code read via
            // Map(depth_buffers_[histIdx]) (outIdx@N-2 == histIdx@N), minus
            // the CPU-GPU sync point and the Dawn shared-memory clobber.
            // WASM keeps its fallback only when the ring is unavailable.
            const bool isWasm = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
            const bool ringReady = depth_ring_copies_ > 2 && depth_readback_ring_.is_initialized();
            const bool varianceFenced=VarianceFenceEnabled(device_);
            if(varianceFenced)VarianceFenceSceneStillCurrent();
            auto* cpuHistory=cpu_stall_audit::NewHistory();
            if(cpuHistory) {cpu_stall_audit::VolumeName(*cpuHistory,rg_name_prefix_.empty()?"Near":rg_name_prefix_.c_str());
                cpuHistory->probe_count=probeCountTotal;cpuHistory->volume_frame=camera_data.frame_index;cpuHistory->slot=frameIdx;cpuHistory->first=isFirstFrame;
                cpuHistory->far_policy=farPolicy;cpuHistory->copies=depth_ring_copies_;cpuHistory->rays=params_.rays_per_probe;
                cpuHistory->variance_fenced=varianceFenced;cpuHistory->variance_confirmed=variance_cpu_confirmed_;cpuHistory->variance_skip=5;
                cpuHistory->mode=volume_data_.RaySampleMode;cpuHistory->budget=max_probes_per_frame_;cpuHistory->gi_frame=camera_data.gi_frame_index;
                cpuHistory->delta_bits=std::bit_cast<u32>(camera_data.delta_time);cpuHistory->volume_uploaded=volumeUploaded;
                cpuHistory->warmup=GIWarmupFrames()>0u && camera_data.gi_frame_index<GIWarmupFrames();}
            auto segmentStart=cpu_stall_audit::state.active && cpu_stall_audit::Timing()?cpu_stall_audit::Clock::now():cpu_stall_audit::Clock::time_point{};
            auto segment=[&](const char* name) {if(cpu_stall_audit::state.active && cpu_stall_audit::Timing()) {auto now=cpu_stall_audit::Clock::now();cpu_stall_audit::Add(name,std::chrono::duration<double,std::milli>(now-segmentStart).count());segmentStart=now;}};
            if(cpuHistory)cpuHistory->ready=ringReady;
            const bool varianceScheduled=!farPolicy && (!ringReady ? !isWasm : true) && !isFirstFrame &&
                probe_states_.size() == probeCountTotal && probe_update_list_buffers_[frameIdx] != handles::INVALID_RESOURCE;
            if(varianceFenced && VarianceFenceAuditEnabled() && !varianceScheduled)++variance_audit_.skip_counts[5];
            if (varianceScheduled) {
                const u32 rbSlot = varianceFenced ? variance_fence_ticket_.Slot() : depth_readback_ring_.slot_for_frame(camera_data.frame_index - 2u);
                const VarianceFenceTicket::Entry* certified=nullptr;
                bool sourceMismatch=false;
                if(varianceFenced && ringReady && rbSlot<3) {
                    certified=variance_fence_ticket_.Take(depth_readback_ring_.buffer(rbSlot),(u64)probeCountTotal*128u*sizeof(float));
                    if(certified && (certified->scene_epoch!=variance_scene_epoch_ || certified->source_digest!=variance_source_digest_ ||
                        certified->source_generation!=variance_generation_ || certified->source_frame>u64(~u32(0)) ||
                        certified->source!=depth_buffers_[DDGISlotsForFrame(u32(certified->source_frame)).writeIndex])){sourceMismatch=true;certified=nullptr;}
                }
                if(cpuHistory) {cpuHistory->variance_skip=varianceFenced?(!ringReady?6:rbSlot>=3?1:sourceMismatch?3:certified?0:2):0;
                    if(certified){cpuHistory->variance_source_frame=u32(certified->source_frame);cpuHistory->variance_source_native=certified->context.frame;cpuHistory->variance_source_ticket=certified->context.ticket;cpuHistory->variance_completed_serial=device_->FrameSlotCompletions().Serial(rbSlot);}
                    cpuHistory->ring_slot=rbSlot;cpuHistory->ring_recorded=rbSlot<3?depth_ring_recorded_frame_[rbSlot]:~u32(0);
                    cpuHistory->completion_serial=device_->FrameSlotCompletions().Serial(rbSlot);cpuHistory->ring_recorded_native=rbSlot<3?depth_ring_recorded_native_[rbSlot]:~u64(0);}
                segmentStart=cpu_stall_audit::state.active && cpu_stall_audit::Timing()?cpu_stall_audit::Clock::now():cpu_stall_audit::Clock::time_point{};
                float* depthData = ringReady && (!varianceFenced || certified)
                    ? static_cast<float*>(depth_readback_ring_.map(device_, rbSlot))
                    : nullptr;
                segment("ddgi.variance_map");
                if(cpuHistory){cpuHistory->map_ok=depthData!=nullptr;
                    if(varianceFenced && certified && !depthData)cpuHistory->variance_skip=4;
                    cpuHistory->variance_accepted=varianceFenced && certified && depthData;}
                if(varianceFenced && VarianceFenceAuditEnabled()) {
                    const u32 skip=!ringReady?6:rbSlot>=3?1:sourceMismatch?3:!certified?2:!depthData?4:0;
                    ++variance_audit_.skip_counts[skip];
                    if(skip==0) {
                        variance_audit_.accepted_frame=true;++variance_audit_.accepted_count;
                        variance_audit_.source_frame=certified->source_frame;variance_audit_.source_native=certified->context.frame;
                        variance_audit_.source_ticket=certified->context.ticket;variance_audit_.completion_serial=device_->FrameSlotCompletions().Serial(rbSlot);
                        variance_audit_.source_epoch=certified->scene_epoch;variance_audit_.source_digest=certified->source_digest;
                        variance_audit_.source_generation=certified->source_generation;
                    }
                }
                if (depthData) {
                    static constexpr u32 DDGI_DEPTH_TEXELS = 64;
                    static constexpr u32 DDGI_FLOATS_PER_PROBE = DDGI_DEPTH_TEXELS * 2;
                    for (u32 i = 0; i < probeCountTotal; ++i) {
                        float maxVar = 0.0f;
                        for (u32 o = 0; o < DDGI_DEPTH_TEXELS; ++o) {
                            maxVar = std::max(maxVar, depthData[i * DDGI_FLOATS_PER_PROBE + DDGI_DEPTH_TEXELS + o]);
                        }
                        probe_states_[i].max_depth_variance = maxVar;
                    }
                    if(varianceFenced)variance_cpu_confirmed_=true;
                    if(cpuHistory)cpuHistory->variance_confirmed=variance_cpu_confirmed_;
                    segment("ddgi.variance_scan");
                    if (ringReady) depth_readback_ring_.unmap(device_, rbSlot);
                    else device_->UnmapBuffer(depth_buffers_[histIdx]);
                    segment("ddgi.variance_unmap");
                }

                // Compute priority scores and select top-N probes
                float maxExpectedVariance = params_.probe_spacing * params_.probe_spacing;

                struct ProbePriority { u32 index; float score; };
                std::vector<ProbePriority> priorities(probeCountTotal);

                // Extract camera position from inverse view (already computed above)
                math::v3 cameraPos = {
                    0.0f, 0.0f, 0.0f  // placeholder, computed below
                };
                {
                    math::m4x4 invView = rhi::math::Inverse(camera_data.view_matrix);
                    cameraPos = { math::elem(invView, 3, 0), math::elem(invView, 3, 1), math::elem(invView, 3, 2) };
                }

                for (u32 i = 0; i < probeCountTotal; ++i) {
                    // Compute probe world position
                    u32 ix = i % params_.probe_count_x;
                    u32 iy = (i / params_.probe_count_x) % params_.probe_count_y;
                    u32 iz = i / (params_.probe_count_x * params_.probe_count_y);
                    math::v3 probePos = {
                        probe_origin_.x + (float)ix * params_.probe_spacing,
                        probe_origin_.y + (float)iy * params_.probe_spacing,
                        probe_origin_.z + (float)iz * params_.probe_spacing
                    };

                    math::v3 diff = probePos - cameraPos;
                    float dist2 = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                    float distScore = 1.0f / (1.0f + dist2);
                    float varianceScore = std::min(probe_states_[i].max_depth_variance / maxExpectedVariance, 1.0f);
                    // Do not cap age at 30 frames: camera-distance and variance
                    // terms otherwise starve the right-arcade probes for >200
                    // frames. DDGI_AGE_CAP=30 restores the former scheduler.
                    static const bool cappedAge = std::getenv("DDGI_AGE_CAP") != nullptr;
                    float age = float(current_frame_index - probe_states_[i].last_update_frame);
                    float ageScore = (cappedAge ? std::min(age, 30.0f) : age) / 30.0f;

                    priorities[i] = { i, distScore + varianceScore * 0.5f + ageScore * 0.3f };
                    if(cpuHistory) {cpu_stall_audit::Hash(cpuHistory->max_hash,std::bit_cast<u32>(probe_states_[i].max_depth_variance));
                        cpu_stall_audit::Hash(cpuHistory->score_hash,std::bit_cast<u32>(priorities[i].score));
                        cpu_stall_audit::Hash(cpuHistory->age_hash,probe_states_[i].last_update_frame);}
                }

                if(cpuHistory)cpuHistory->scores_valid=true;
                segment("ddgi.priority");
                // Partial sort: top-N only
                updateCount = std::min(max_probes_per_frame_, probeCountTotal);
                std::partial_sort(priorities.begin(), priorities.begin() + updateCount,
                                  priorities.end(), [](const ProbePriority& a, const ProbePriority& b) {
                                      return a.score > b.score;
                                  });

                segment("ddgi.priority_sort");
                // Write update list buffer
                u32* updateList = static_cast<u32*>(device_->MapBuffer(probe_update_list_buffers_[frameIdx]));
                segment("ddgi.list_map");
                if(cpuHistory)cpuHistory->list_ok=updateList!=nullptr;
                if (updateList) {
                    for (u32 i = 0; i < updateCount; ++i) {
                        updateList[i] = priorities[i].index;
                        if(cpuHistory)cpu_stall_audit::Selection(*cpuHistory,updateList[i]);
                        probe_states_[priorities[i].index].last_update_frame = current_frame_index;
                    }
                    segment("ddgi.list_fill");
                    device_->UnmapBuffer(probe_update_list_buffers_[frameIdx]);
                    segment("ddgi.list_unmap");
                }

                // 2026-09-17 P0（probe-selection-density-audit §3.3/§9-P2）：
                // age 分布与更新率一次性实测（DDGI_AGE_STATS=1 开启；审计要求
                // 的"age P95/max 与 update ratio"口径）。检查紧预算下的饥饿：
                // age 封顶 30 帧后长期不更新的点不再提优先级。
                if (std::getenv("DDGI_AGE_STATS")) {
                    static bool ageDumped = false;
                    if (!ageDumped && updateCount < probeCountTotal) {
                        ageDumped = true;
                        std::vector<u32> ages;
                        ages.reserve(probeCountTotal);
                        for (u32 i = 0; i < probeCountTotal; ++i) {
                            ages.push_back(current_frame_index - probe_states_[i].last_update_frame);
                        }
                        std::sort(ages.begin(), ages.end());
                        auto pick = [&](float frac) -> u32 {
                            return ages[std::min(ages.size() - 1,
                                                 (size_t)(ages.size() * frac))];
                        };
                        PRIMAL_LOG_STREAM(Error, "Render")
                            << "[DDGIAgeStats] update_ratio=" << updateCount
                            << "/" << probeCountTotal
                            << " age_med=" << pick(0.5f)
                            << " age_P95=" << pick(0.95f)
                            << " age_max=" << ages.back()
                            << " (capped-30 starvation risk when P95==30 and"
                               " update_ratio<1)"
                            << std::endl;
                    }
                }

                // GI ownership plan Phase 0 (2026-09-18): openSky 直方图一次性
                // dump（DDGI_OPENSKY_STATS=1，沿用上方 age-stats 的"一次即弃"
                // 模式）。收敛窗后逐探针读回 openSky 逃逸占比，10 桶直方图 +
                // 中位数——基线/断边 A/B 都用它回答"探针场的天空归属分布"。
                if (std::getenv("DDGI_OPENSKY_STATS")) {
                    static bool skyDumped = false;
                    static const u32 skyDumpFrame = [] {
                        const char* e = std::getenv("DDGI_OPENSKY_STATS_FRAME");
                        return e ? (u32)std::atoi(e) : 300u;
                    }();
                    if (!skyDumped && current_frame_index >= skyDumpFrame &&
                        opensky_buffer_ != handles::INVALID_RESOURCE) {
                        skyDumped = true;
                        const u32 n = volume_data_.ProbeCountTotal;
                        if (float* os = static_cast<float*>(
                                device_->MapBuffer(opensky_buffer_, 0, (u64)n * 2u * sizeof(float)))) {
                            u32 histBuckets[10] = {};
                            std::vector<float> vals;
                            std::vector<float> sers;
                            vals.reserve(n);
                            sers.reserve(n);
                            for (u32 i = 0; i < n; ++i) {
                                const float v = std::clamp(os[i * 2u + 0u], 0.0f, 1.0f);
                                vals.push_back(v);
                                sers.push_back(std::clamp(os[i * 2u + 1u], 0.0f, 1.0f));
                                histBuckets[std::min(9u, (u32)(v * 10.0f))]++;
                            }
                            device_->UnmapBuffer(opensky_buffer_);
                            std::sort(vals.begin(), vals.end());
                            const float med = vals.empty()
                                ? 0.0f
                                : vals[vals.size() / 2];
                            std::sort(sers.begin(), sers.end());
                            const float serMed = sers.empty()
                                ? 0.0f
                                : sers[sers.size() / 2];
                            std::string hist;
                            for (int b = 0; b < 10; ++b) {
                                char cell[16];
                                std::snprintf(cell, sizeof(cell), "%s%u",
                                              b ? "," : "", (unsigned)histBuckets[b]);
                                hist += cell;
                            }
                            PRIMAL_LOG_STREAM(Error, "Render")
                                << "[DDGIOpenSkyStats] probes=" << n << " med=" << med
                                << " ser_med=" << serMed
                                << " hist(0-1 step0.1)=" << hist << std::endl;
                        }
                    }
                }

                // Patch ProbeUpdateCount in the volume constant buffer
                {
                    auto* mapped = static_cast<DDGIVolumeData*>(device_->MapBuffer(volume_cb_[frameIdx]));
                    if (mapped) {
                        // C3.5 slice 3 (deterministic stabilization): during the
                        // atlas warm-up window the probe field stays frozen at
                        // its deterministic init — updating probes against the
                        // still-converging capture/atlas is what selects the
                        // multi-stable branch (09-10 十二). PRIMAL_SC_WARMUP_
                        // FRAMES gates the freeze; 0 (default) = off.
                        mapped->ProbeUpdateCount = updateCount;
                        device_->UnmapBuffer(volume_cb_[frameIdx]);
                        volume_data_.ProbeUpdateCount = updateCount;
                    }
                }

                // T1.5 (R4): encode the dispatch group count for the
                // DispatchIndirect smoke path (PRIMAL_DDGI_INDIRECT=1).
                // Group encoding matches the platform branches at the depth
                // dispatch below (Metal: one group per probe; Vulkan: 64
                // probes per group; Dawn: workgroups = probes).
                if (probe_dispatch_count_buffer_ != handles::INVALID_RESOURCE) {
                    const bool isMetalPlat = (device_->GetPlatform() == RHIPlatform::Metal);
                    const u32 groups = isMetalPlat ? updateCount : ((updateCount + 63) / 64);
                    if (u32* cnt = static_cast<u32*>(device_->MapBuffer(probe_dispatch_count_buffer_))) {
                        cnt[0] = groups; cnt[1] = 1; cnt[2] = 1; cnt[3] = 0;
                        device_->UnmapBuffer(probe_dispatch_count_buffer_);
                    }
                }
            }

            // First frame: fill update list with all probe indices (0..N-1)
            if (!farPolicy && isFirstFrame && probe_update_list_buffers_[frameIdx] != handles::INVALID_RESOURCE) {
                segmentStart=cpu_stall_audit::state.active && cpu_stall_audit::Timing()?cpu_stall_audit::Clock::now():cpu_stall_audit::Clock::time_point{};
                u32* updateList = static_cast<u32*>(device_->MapBuffer(probe_update_list_buffers_[frameIdx]));
                segment("ddgi.list_map");
                if(cpuHistory){cpuHistory->list_ok=updateList!=nullptr;cpuHistory->updates=updateCount;}
                if (updateList) {
                    for (u32 i = 0; i < updateCount; ++i) {
                        updateList[i] = i;
                        if(cpuHistory)cpu_stall_audit::Selection(*cpuHistory,updateList[i]);
                    }
                    segment("ddgi.list_fill");
                    device_->UnmapBuffer(probe_update_list_buffers_[frameIdx]);
                    segment("ddgi.list_unmap");
                }
            }

            if(farPolicy) {
                if(!volumeUploaded) {volume_data_.ProbeUpdateCount=0;return;}
                segmentStart=cpu_stall_audit::state.active && cpu_stall_audit::Timing()?cpu_stall_audit::Clock::now():cpu_stall_audit::Clock::time_point{};
                auto* list=static_cast<u32*>(device_->MapBuffer(probe_update_list_buffers_[frameIdx]));
                segment("ddgi.list_map");
                if(cpuHistory){cpuHistory->list_ok=list!=nullptr;cpuHistory->updates=updateCount;}
                if(!list) {volume_data_.ProbeUpdateCount=0;return;}
                std::memcpy(list,farPlan.indices.data(),updateCount*sizeof(u32));
                if(cpuHistory)for(u32 i=0;i<updateCount;++i)cpu_stall_audit::Selection(*cpuHistory,farPlan.indices[i]);
                segment("ddgi.list_fill");
                device_->UnmapBuffer(probe_update_list_buffers_[frameIdx]);
                segment("ddgi.list_unmap");
                // Preflight the finalizer uniform upload before any trace writes.
                auto* vp=static_cast<FinalizeViewData*>(device_->MapBuffer(finalize_vp_cb_[frameIdx]));
                if(!vp) {volume_data_.ProbeUpdateCount=0;return;}
                vp->currentVP=current_view_projection_;
                vp->radianceVP=camera_data.radiance_is_previous_frame
                    ? camera_data.prev_proj_matrix*camera_data.prev_view_matrix : current_view_projection_;
                vp->inverseCurrentVP=rhi::math::Inverse(vp->currentVP);
                vp->inverseRadianceVP=rhi::math::Inverse(vp->radianceVP);
                vp->validity=math::v4{screenDepthTex!=handles::INVALID_RESOURCE?1.f:0.f,
                    radianceDepthTex!=handles::INVALID_RESOURCE?1.f:0.f,.15f,0.f};
                device_->UnmapBuffer(finalize_vp_cb_[frameIdx]);
                far_update_audit_.uploaded=true;
                far_update_audit_.list_hash=FarUpdatePolicy::Hash(farPlan);
                cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,
                    AccessFlag::HostWrite,AccessFlag::ShaderRead | AccessFlag::UniformRead);
            }

            // Explicit bounded diagnostics: only selected probes, and only on
            // selected capture frames. Ray slots outside the update prefix are stale.
            std::vector<u32> historyProbes;
            const char* historySelection=std::getenv("DDGI_HISTORY_PROBES");
            if (historySelection && *historySelection && rg_name_prefix_.empty() &&
                device_->GetPlatform()==RHIPlatform::Vulkan && lumencompare::enabled &&
                lumencompare::Selected("DDGI.History")) {
                std::istringstream input(historySelection); std::string token;
                bool valid=historySelection[std::strlen(historySelection)-1]!=',';
                while (std::getline(input,token,',')) {
                    if (token.empty() || token.size()>9 ||
                        token.find_first_not_of("0123456789")!=std::string::npos) {valid=false;break;}
                    const auto pid=std::strtoul(token.c_str(),nullptr,10);
                    if (pid>=probeCountTotal || historyProbes.size()>=16 ||
                        std::find(historyProbes.begin(),historyProbes.end(),u32(pid))!=historyProbes.end()) {valid=false;break;}
                    historyProbes.push_back(u32(pid));
                }
                if (!valid) {
                    historyProbes.clear();
                    PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI.History] invalid probe selection" << std::endl;
                }
            }
            const auto historySlice=[&](const char* stage, const char* label, ResourceHandle buffer, u64 stride) {
                for (u32 pid:historyProbes)
                    lumencompare::BufferSlice(device_,cmd,stage,std::string(label)+"-"+std::to_string(pid),buffer,u64(pid)*stride,stride);
            };
            if (!historyProbes.empty()) {
                historySlice("DDGI.History.PreTrace","origin",relocated_origins_,16);
                historySlice("DDGI.History.PreTrace","depth",depth_buffers_[histIdx],512);
                historySlice("DDGI.History.PreTrace","sh",irradiance_buffers_[histIdx],144);
                if (updateCount) lumencompare::BufferSlice(device_,cmd,"DDGI.History.PreTrace","update-list",
                    probe_update_list_buffers_[frameIdx],0,u64(updateCount)*sizeof(u32));
                const auto directory=std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR"))/("frame-"+std::to_string(lumencompare::ordinal));
                std::filesystem::create_directories(directory);
                std::ofstream meta(directory/"ddgi-history-near.json");
                meta << "{\"ordinal\":" << lumencompare::ordinal << ",\"native_frame\":" << current_frame_index
                     << ",\"descriptor_slot\":" << frameIdx << ",\"history_slot\":" << histIdx << ",\"write_slot\":" << outIdx
                     << ",\"update_count\":" << updateCount << ",\"probe_count\":" << probeCountTotal
                     << ",\"rays_per_probe\":" << params_.rays_per_probe
                     << ",\"history_handle\":" << depth_buffers_[histIdx] << ",\"output_handle\":" << depth_buffers_[outIdx]
                     << ",\"origins_handle\":" << relocated_origins_ << ",\"scene_epoch\":" << measured_geometry_epoch_
                     << ",\"source_digest\":" << measured_geometry_digest_ << ",\"owner_directory_ready\":" << (measured_owner_directory_ready_?"true":"false")
                     << ",\"geometry_handles\":[";
                for (u32 i=0;i<4;++i) meta << (i?",":"") << measured_geometry_[i];
                meta << "],\"probes\":[";
                for (size_t i=0;i<historyProbes.size();++i) meta << (i?",":"") << historyProbes[i];
                meta << "],\"validity\":\"Only update-list prefix produces observations; offsets and sizes are in capture.json; origin and scene identity are evidence to compare, not implicit certification\"}\n";
            }

            // Sparse updates must begin from the complete previous probe field.
            // The update kernels only overwrite probes in probeUpdateList; without
            // this inheritance, untouched probes retain whatever happened to be
            // in outIdx three frames ago and the rotating buffers develop three
            // different brightness states.
            if (!farPolicy && updateCount < probeCountTotal) {
                const u64 irradianceBytes = (u64)probeCountTotal * 9u * sizeof(math::v4);
                static constexpr u32 kDepthFloatsPerProbe = 64u * 2u;
                const u64 depthBytes = (u64)probeCountTotal * kDepthFloatsPerProbe * sizeof(float);
                cmd->CopyBuffer(irradiance_buffers_[histIdx], irradiance_buffers_[outIdx],
                                0, 0, irradianceBytes);
                cmd->CopyBuffer(depth_buffers_[histIdx], depth_buffers_[outIdx],
                                0, 0, depthBytes);
                if (depth_provenance_enabled_) {
                    ResourceBarrier barriers[2]{};
                    barriers[0].resource=depth_provenance_buffers_[histIdx];
                    barriers[0].beforeState=ResourceState::ShaderResource;barriers[0].afterState=ResourceState::CopySource;
                    barriers[1].resource=depth_provenance_buffers_[outIdx];
                    barriers[1].beforeState=ResourceState::UnorderedAccess;barriers[1].afterState=ResourceState::CopyDest;
                    cmd->InsertBarrier(barriers,2);
                    cmd->CopyBuffer(depth_provenance_buffers_[histIdx],depth_provenance_buffers_[outIdx],
                        0,0,u64(probeCountTotal)*64u*8u*sizeof(u32));
                    for (auto& b:barriers) std::swap(b.beforeState,b.afterState);
                    cmd->InsertBarrier(barriers,2);
                }
                cmd->MemoryBarrier(
                    PipelineStage::Transfer, PipelineStage::ComputeShader,
                    AccessFlag::TransferWrite,
                    AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
            }

            _t3 = chr::steady_clock::now();
            // ================================================================
            // Sub-pass 1: TraceRays — split into SDF trace + finalize
            // Apple Silicon: separate texture3D reads from buffer reads
            // ================================================================
            if (sdfAvailable &&
                sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
                finalize_pipeline_ != handles::INVALID_PIPELINE &&
                hit_distance_buffer_ != handles::INVALID_RESOURCE) {

                // --- 1a: SDF trace (texture3D-only, 4 steps) ---
                {
                    const bool splitIsVk =
                        (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
                    const bool useFirstSurface = splitIsVk && first_surface_enabled_ &&
                        !first_zero_cross_enabled_ &&
                        first_surface_sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
                        first_surface_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT &&
                        first_surface_nodes_buffer_ != handles::INVALID_RESOURCE &&
                        first_surface_triangles_buffer_ != handles::INVALID_RESOURCE &&
                        first_surface_audit_buffer_ != handles::INVALID_RESOURCE;
                    const bool useFirstZeroCross = splitIsVk && first_zero_cross_enabled_ &&
                        first_zero_cross_sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
                        first_surface_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT &&
                        first_surface_sdf_trace_ds_[frameIdx] != handles::INVALID_DESCRIPTOR_SET &&
                        first_surface_nodes_buffer_ != handles::INVALID_RESOURCE &&
                        first_surface_triangles_buffer_ != handles::INVALID_RESOURCE &&
                        first_surface_audit_buffer_ != handles::INVALID_RESOURCE;
                    const bool useFirstNativeNormal = splitIsVk && first_native_normal_enabled_ &&
                        first_surface_enabled_ &&
                        first_native_normal_sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
                        first_native_normal_sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT &&
                        first_native_normal_sdf_trace_ds_[frameIdx] != handles::INVALID_DESCRIPTOR_SET &&
                        first_native_normal_positions_ != handles::INVALID_RESOURCE &&
                        first_native_normal_elements_ != handles::INVALID_RESOURCE &&
                        first_native_normal_instances_ != handles::INVALID_RESOURCE &&
                        first_native_normal_materials_ != handles::INVALID_RESOURCE &&
                        first_native_normal_texture_ != handles::INVALID_RESOURCE &&
                        first_native_normal_sampler_ != handles::INVALID_SAMPLER &&
                        first_native_normal_triangle_mapping_buffer_ != handles::INVALID_RESOURCE &&
                        first_native_normal_audit_buffer_ != handles::INVALID_RESOURCE;
                    // T4.3: upload the detail slot table into this frame's
                    // UBO slot (656B; count=0 when the feature is off — the
                    // shader then skips refinement and writes zero normals).
                    if (detail_table_cb_[frameIdx] != handles::INVALID_RESOURCE) {
                        struct DetailTableGPU {
                            math::v4 header;                    // x = slot count
                            math::m4x4 inv_world[4];
                            math::v4 normal_matrix[4][3];
                            math::v4 bounds_min[4];
                            math::v4 bounds_max[4];
                            math::v4 params[4];                 // res, max_dim, signed, voxel
                        };
                        static_assert(sizeof(DetailTableGPU) == 656, "detail table layout drift");
                        auto* tm = static_cast<DetailTableGPU*>(device_->MapBuffer(detail_table_cb_[frameIdx]));
                        if(farPolicy && !tm) {volume_data_.ProbeUpdateCount=0; return;}
                        if (tm) {
                            tm->header = math::v4{(f32)detail_sdf_slot_count_, 0.f, 0.f, 0.f};
                            for (u32 s = 0; s < 4; ++s) {
                                if (s < detail_sdf_slot_count_) {
                                    tm->inv_world[s] = detail_sdf_slots_[s].inv_world;
                                    for (u32 r = 0; r < 3; ++r)
                                        tm->normal_matrix[s][r] = detail_sdf_slots_[s].normal_matrix[r];
                                    tm->bounds_min[s] = detail_sdf_slots_[s].bounds_min;
                                    tm->bounds_max[s] = detail_sdf_slots_[s].bounds_max;
                                    tm->params[s] = detail_sdf_slots_[s].params;
                                } else {
                                    tm->inv_world[s] = math::m4x4{};
                                    tm->normal_matrix[s][0] = tm->normal_matrix[s][1] = math::v4{};
                                    tm->normal_matrix[s][2] = math::v4{};
                                    tm->bounds_min[s] = tm->bounds_max[s] = math::v4{};
                                    tm->params[s] = math::v4{};
                                }
                            }
                            device_->UnmapBuffer(detail_table_cb_[frameIdx]);
                        }
                    }

                    if(farPolicy) cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,
                        AccessFlag::HostWrite,AccessFlag::ShaderRead | AccessFlag::UniformRead);

                    // Detail textures: real slots, unused slots fall back to
                    // the (always valid, always 3D) SDF cascade texture — the
                    // shader never samples them when slot count is 0, and a
                    // dummy 3D texture dependency is removed entirely.
                    ResourceHandle detailTex[kDetailSDFSlots];
                    for (u32 s = 0; s < kDetailSDFSlots; ++s) {
                        detailTex[s] = (s < detail_sdf_slot_count_ &&
                                        detail_sdf_textures_[s] != handles::INVALID_RESOURCE)
                            ? detail_sdf_textures_[s] : sdfTextures[0];
                    }

                    // Flat bindings on Vulkan (UBO=3, hit_dist=4, list=5);
                    // Metal keeps overlapping slots (UBO=1, hit_dist=2, list=3).
                    // T4.3: hit_normal + 4 detail textures + table UBO appended
                    // (Vulkan flat 6..11; Metal buffer 4/5 + texture 3..6).
                    DescriptorData sdfTraceParams[24] = {
                        {0, DescriptorType::SampledImage,  sdfTextures[0]},
                        {1, DescriptorType::SampledImage,  sdfTextures[1]},
                        {2, DescriptorType::SampledImage,  sdfTextures[2]},
                        {splitIsVk ? 3u : 1u, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                        {splitIsVk ? 4u : 2u, DescriptorType::StorageBuffer, hit_distance_buffer_},
                        {splitIsVk ? 5u : 3u, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                        {splitIsVk ? 6u : 4u, DescriptorType::StorageBuffer, hit_normal_buffer_},
                        {splitIsVk ? 7u : 3u, DescriptorType::SampledImage,  detailTex[0]},
                        {splitIsVk ? 8u : 4u, DescriptorType::SampledImage,  detailTex[1]},
                        {splitIsVk ? 9u : 5u, DescriptorType::SampledImage,  detailTex[2]},
                        {splitIsVk ? 10u : 6u, DescriptorType::SampledImage, detailTex[3]},
                        {splitIsVk ? 11u : 5u, DescriptorType::UniformBuffer, detail_table_cb_[frameIdx]},
                        // 2026-09-17 P0：重定位后有效起点（本 pass 写，finalize/gather 读）
                        {splitIsVk ? 12u : 6u, DescriptorType::StorageBuffer, relocated_origins_},
                    };
                    if (measured_geometry_requested_) {
                        cmd->MemoryBarrier(PipelineStage::Host, PipelineStage::ComputeShader,
                                           AccessFlag::HostWrite, AccessFlag::ShaderRead);
                        for (u32 i=0;i<4;++i) sdfTraceParams[13+i]={13+i,DescriptorType::StorageBuffer,
                            measured_geometry_[i]!=handles::INVALID_RESOURCE ? measured_geometry_[i] : probe_update_list_buffers_[frameIdx]};
                        UpdateDescriptorSet(device_,sdf_trace_ds_[frameIdx],sdfTraceParams,17);
                    } else if (useFirstNativeNormal || useFirstSurface || useFirstZeroCross) {
                        // The immutable BVH upload is a host/transfer producer;
                        // make that dependency explicit before the compute
                        // shader reads the new storage buffers.  The ordinary
                        // InsertBarrier path is texture-only on Vulkan.
                        cmd->MemoryBarrier(
                            PipelineStage::Host | PipelineStage::Transfer,
                            PipelineStage::ComputeShader,
                            AccessFlag::HostWrite | AccessFlag::TransferWrite,
                            AccessFlag::ShaderRead);
                        sdfTraceParams[13] = {13, DescriptorType::StorageBuffer,
                                              first_surface_nodes_buffer_};
                        sdfTraceParams[14] = {14, DescriptorType::StorageBuffer,
                                              first_surface_triangles_buffer_};
                        sdfTraceParams[15] = {15, DescriptorType::StorageBuffer,
                                              first_surface_audit_buffer_};
                        if (useFirstNativeNormal) {
                            sdfTraceParams[16] = {16, DescriptorType::StorageBuffer,
                                                  first_native_normal_positions_};
                            sdfTraceParams[17] = {17, DescriptorType::StorageBuffer,
                                                  first_native_normal_elements_};
                            sdfTraceParams[18] = {18, DescriptorType::StorageBuffer,
                                                  first_native_normal_instances_};
                            sdfTraceParams[19] = {19, DescriptorType::StorageBuffer,
                                                  first_native_normal_materials_};
                            sdfTraceParams[20] = {20, DescriptorType::StorageBuffer,
                                                  first_native_normal_triangle_mapping_buffer_};
                            sdfTraceParams[21] = {21, DescriptorType::SampledImage,
                                                  first_native_normal_texture_};
                            sdfTraceParams[22] = {22, DescriptorType::Sampler,
                                                  static_cast<ResourceHandle>(first_native_normal_sampler_)};
                            sdfTraceParams[23] = {23, DescriptorType::StorageBuffer,
                                                  first_native_normal_audit_buffer_};
                            UpdateDescriptorSet(device_, first_native_normal_sdf_trace_ds_[frameIdx],
                                                sdfTraceParams, 24);
                        } else {
                            UpdateDescriptorSet(device_, first_surface_sdf_trace_ds_[frameIdx],
                                                sdfTraceParams, 16);
                        }
                    } else {
                        UpdateDescriptorSet(device_, sdf_trace_ds_[frameIdx],
                                            sdfTraceParams, 13);
                    }

                    // barrier: 上一帧 irradiance/depth 写入对本帧 trace 可见
                    cmd->MemoryBarrier(
                        PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                        AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                    const PipelineHandle tracePipeline = useFirstNativeNormal
                        ? first_native_normal_sdf_trace_pipeline_
                        : (useFirstZeroCross ? first_zero_cross_sdf_trace_pipeline_
                                             : (useFirstSurface ? first_surface_sdf_trace_pipeline_
                                                                 : sdf_trace_pipeline_));
                    const PipelineLayoutHandle traceLayout = useFirstNativeNormal
                        ? first_native_normal_sdf_trace_layout_
                        : (useFirstZeroCross ? first_surface_sdf_trace_layout_
                                             : (useFirstSurface ? first_surface_sdf_trace_layout_
                                                                 : sdf_trace_layout_));
                    const DescriptorSetHandle traceSet = useFirstNativeNormal
                        ? first_native_normal_sdf_trace_ds_[frameIdx]
                        : ((useFirstZeroCross || useFirstSurface)
                               ? first_surface_sdf_trace_ds_[frameIdx]
                               : sdf_trace_ds_[frameIdx]);
                    cmd->BindComputePipeline(tracePipeline);
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, traceLayout,
                                            0, 1, &traceSet, 0, nullptr);

                    cmd->Dispatch(updateCount, 1, 1);
                    if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.TraceSDF", rg_name_prefix_+"hit-distance", hit_distance_buffer_, true);
                    if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.TraceSDF", rg_name_prefix_+"hit-normal", hit_normal_buffer_, true);
                }

                // Barrier: end encoder for strict Apple Silicon synchronization.
                // memoryBarrier() alone may not flush GPU L2 cache for heap-allocated
                // StorageModeShared buffers. Ending the encoder guarantees visibility.
                // Vulkan's InsertBarrier intentionally skips buffers, so the
                // compute memory barrier is the real TraceSDF -> Finalize
                // dependency for hit_distance, hit_normal and the optional
                // ray-source tail. Finalize both reads and writes that SSBO.
                const AccessFlag finalizeAccess =
                    (measured_geometry_requested_ || analytic_audit_enabled_ || first_surface_enabled_ || first_zero_cross_enabled_ || first_native_normal_enabled_ ||
                     analytic_first_rho_enabled_)
                    ? (AccessFlag::ShaderRead | AccessFlag::ShaderWrite)
                    : AccessFlag::ShaderRead;
                cmd->MemoryBarrier(
                    PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                    AccessFlag::ShaderWrite, finalizeAccess);

                // --- 1b: Finalize (buffer + texture2D, no texture3D) ---
                {
                    // Vulkan: primary radiance = prev_frame_color (screen-space,
                    // has real material colors); SC atlas = fallback for
                    // off-screen hits. Metal keeps the SC-or-prev logic.
                    ResourceHandle lightingAtlasTex = sc_enabled_
                        ? sc_lighting_atlas_
                        : prevColorTex;

                    // Guard against null texture binding — nil texture in compute shader
                    // causes GPU hang on Apple Silicon. Skip finalize if no valid source.
                    if (lightingAtlasTex != handles::INVALID_RESOURCE) {

                    const bool finIsVk =
                        (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
                    const bool useAnalyticFirstRho = finIsVk &&
                        analytic_first_rho_enabled_ &&
                        analytic_first_rho_finalize_pipeline_ != handles::INVALID_PIPELINE &&
                        analytic_first_rho_finalize_layout_ != handles::INVALID_PIPELINE_LAYOUT &&
                        analytic_first_rho_finalize_ds_[frameIdx] != handles::INVALID_DESCRIPTOR_SET &&
                        analytic_first_rho_nodes_buffer_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_triangles_buffer_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_mapping_buffer_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_audit_buffer_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_positions_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_elements_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_instances_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_materials_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_albedo_texture_ != handles::INVALID_RESOURCE &&
                        analytic_first_rho_albedo_sampler_ != handles::INVALID_SAMPLER;

                    // Match each screen source to its own projection and depth.
                    if (!farPolicy && finIsVk && finalize_vp_cb_[frameIdx] != handles::INVALID_RESOURCE) {
                        auto* vpm = static_cast<FinalizeViewData*>(
                            device_->MapBuffer(finalize_vp_cb_[frameIdx]));
                        if (vpm) {
                            vpm->currentVP = current_view_projection_;
                            vpm->radianceVP = camera_data.radiance_is_previous_frame
                                ? camera_data.prev_proj_matrix * camera_data.prev_view_matrix
                                : current_view_projection_;
                            vpm->inverseCurrentVP = rhi::math::Inverse(vpm->currentVP);
                            vpm->inverseRadianceVP = rhi::math::Inverse(vpm->radianceVP);
                            vpm->validity = math::v4{
                                screenDepthTex != handles::INVALID_RESOURCE ? 1.0f : 0.0f,
                                radianceDepthTex != handles::INVALID_RESOURCE ? 1.0f : 0.0f,
                                0.15f, 0.0f};
                            device_->UnmapBuffer(finalize_vp_cb_[frameIdx]);
                        }
                    }

                    // Flat bindings on Vulkan; Metal overlap.
                    if (finIsVk && useAnalyticFirstRho) {
                        // Candidate-only host/transfer producer dependency.
                        // Vulkan InsertBarrier intentionally does not certify
                        // buffer contents; this explicit memory barrier is the
                        // contract for the uploaded BVH/mapping and audit SSBO.
                        cmd->MemoryBarrier(
                            PipelineStage::Host | PipelineStage::Transfer,
                            PipelineStage::ComputeShader,
                            AccessFlag::HostWrite | AccessFlag::TransferWrite,
                            AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
                        DescriptorData analyticParams[] = {
                            {0,  DescriptorType::SampledImage,  lightingAtlasTex},
                            {1,  DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                            {2,  DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {3,  DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                            {4,  DescriptorType::StorageBuffer, ray_data_buffer_},
                            {5,  DescriptorType::StorageBuffer, sc_card_lookup_buffer_},
                            {6,  DescriptorType::StorageBuffer, sc_card_data_buffer_},
                            {7,  DescriptorType::UniformBuffer, finalize_vp_cb_[frameIdx]},
                            {8,  DescriptorType::SampledImage,  prevColorTex},
                            {9,  DescriptorType::SampledImage,  gbufferAlbedoTex},
                            {10, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                            {11, DescriptorType::StorageBuffer, sc_grid_offsets_buffer_},
                            {12, DescriptorType::StorageBuffer, sc_grid_cards_buffer_},
                            {13, DescriptorType::SampledImage,  sc_depth_atlas_},
                            {14, DescriptorType::StorageBuffer, hit_normal_buffer_},
                            {15, DescriptorType::SampledImage, screenDepthTex != handles::INVALID_RESOURCE ? screenDepthTex : prevColorTex},
                            {16, DescriptorType::SampledImage, radianceDepthTex != handles::INVALID_RESOURCE ? radianceDepthTex : prevColorTex},
                            {17, DescriptorType::StorageBuffer, relocated_origins_},
                            {18, DescriptorType::StorageBuffer, analytic_first_rho_nodes_buffer_},
                            {19, DescriptorType::StorageBuffer, analytic_first_rho_triangles_buffer_},
                            {20, DescriptorType::StorageBuffer, analytic_first_rho_positions_},
                            {21, DescriptorType::StorageBuffer, analytic_first_rho_elements_},
                            {22, DescriptorType::StorageBuffer, analytic_first_rho_instances_},
                            {23, DescriptorType::StorageBuffer, analytic_first_rho_materials_},
                            {24, DescriptorType::StorageBuffer, analytic_first_rho_mapping_buffer_},
                            {25, DescriptorType::SampledImage,  analytic_first_rho_albedo_texture_},
                            {26, DescriptorType::Sampler, static_cast<ResourceHandle>(analytic_first_rho_albedo_sampler_)},
                            {27, DescriptorType::StorageBuffer, analytic_first_rho_audit_buffer_},
                        };
                        UpdateDescriptorSet(device_, analytic_first_rho_finalize_ds_[frameIdx],
                                            analyticParams, 28);
                    } else if (finIsVk) {
                        DescriptorData finalizeParams[] = {
                            {0, DescriptorType::SampledImage,  lightingAtlasTex},
                            {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                            {2, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {3, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, ray_data_buffer_},
                            {5, DescriptorType::StorageBuffer, sc_card_lookup_buffer_},
                            {6, DescriptorType::StorageBuffer, sc_card_data_buffer_},
                            {7, DescriptorType::UniformBuffer, finalize_vp_cb_[frameIdx]},
                            {8, DescriptorType::SampledImage,  prevColorTex},
                            {9, DescriptorType::SampledImage,  gbufferAlbedoTex},
                            {10, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                            {11, DescriptorType::StorageBuffer, sc_grid_offsets_buffer_},
                            {12, DescriptorType::StorageBuffer, sc_grid_cards_buffer_},
                            {13, DescriptorType::SampledImage,  sc_depth_atlas_},   // T3.4 (D3)
                            {14, DescriptorType::StorageBuffer, hit_normal_buffer_}, // T4.3 (read)
                            {15, DescriptorType::SampledImage, screenDepthTex != handles::INVALID_RESOURCE ? screenDepthTex : prevColorTex},
                            {16, DescriptorType::SampledImage, radianceDepthTex != handles::INVALID_RESOURCE ? radianceDepthTex : prevColorTex},
                            {17, DescriptorType::StorageBuffer, relocated_origins_},   // P0: relocated origins (read)
                        };
                        UpdateDescriptorSet(device_, finalize_ds_[frameIdx], finalizeParams, 18);
                        // Source attribution observes the ordinary near finalizer,
                        // not a replacement shader. Only the requested native F2/F3
                        // executions (display ordinals 3/4) expose these snapshots.
                        if (lumencompare::enabled && lumencompare::ordinal <= 6 &&
                            (lumencompare::ordinal == 3 || lumencompare::ordinal == 4) &&
                            rg_name_prefix_.empty() &&
                            lumencompare::Selected("DDGI.SourceInputsNear")) {
                            struct SourceInput { u32 binding; const char* name; bool buffer; };
                            const SourceInput sources[] = {
                                {0, "sc-lighting", false}, {13, "sc-depth", false},
                                {6, "card-data", true}, {5, "card-lookup", true},
                                {11, "grid-offsets", true}, {12, "grid-cards", true},
                                {7, "view-params", true}, {8, "prev-screen-radiance", false},
                                {9, "gbuffer-albedo", false}, {15, "screen-depth", false},
                                {16, "radiance-depth", false}, {10, "SH-history", true},
                                {1, "volume", true}, {3, "update-list", true},
                                {17, "relocated-origins", true}, {2, "hit-distance", true}
                            };
                            for (const auto& source : sources) {
                                const auto& bound = finalizeParams[source.binding];
                                lumencompare::Output(device_, cmd, "DDGI.SourceInputsNear", source.name,
                                    bound.resource, source.buffer,
                                    source.buffer ? ResourceState::UnorderedAccess : ResourceState::ShaderResource);
                            }
                            // The global UBO is supplemental frame context, not a
                            // bound descriptor of this split-finalize shader.
                            lumencompare::Output(device_, cmd, "DDGI.SourceInputsNear", "global", global_cb_[frameIdx], true);
                        }
                        if (lumencompare::enabled && lumencompare::ordinal <= 6 &&
                            (lumencompare::ordinal == 3 || lumencompare::ordinal == 4) &&
                            rg_name_prefix_.empty() &&
                            (lumencompare::Selected("DDGI.SourceInputsNear") ||
                             lumencompare::Selected("DDGI.SourceWitnessNear"))) {
                            const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                                ("frame-" + std::to_string(lumencompare::ordinal));
                            std::filesystem::create_directories(directory);
                            std::ofstream meta(directory / "source-audit-near.json");
                            const u64 n = u64(probeCountTotal) * params_.rays_per_probe;
                            const u32 normalMultiplier = query_source_audit_enabled_ ? 27u :
                                (ray_source_audit_enabled_ ? 9u : (analytic_audit_enabled_ ? 5u : 1u));
                            meta << "{\"frame\":" << lumencompare::ordinal << ",\"native_frame\":" << current_frame_index
                                 << ",\"descriptor_slot\":" << frameIdx << ",\"write_slot\":" << outIdx
                                 << ",\"history_slot\":" << histIdx << ",\"probe_count\":" << probeCountTotal
                                 << ",\"rays_per_probe\":" << params_.rays_per_probe << ",\"update_count\":" << updateCount
                                 << ",\"N\":" << n << ",\"normal_vec4_multiplier\":" << normalMultiplier
                                 << ",\"normal_expected_bytes\":" << n * normalMultiplier * 16u
                                 << ",\"analytic_vec4_offset\":" << n << ",\"source_vec4_offset\":" << 5u * n
                                 << ",\"query_vec4_offset\":" << 9u * n
                                 << ",\"analytic_records_per_ray\":4,\"source_records_per_ray\":4,\"query_records_per_ray\":18"
                                 << ",\"source_audit_enabled\":" << (ray_source_audit_enabled_ ? "true" : "false")
                                 << ",\"query_audit_enabled\":" << (query_source_audit_enabled_ ? "true" : "false")
                                 << ",\"analytic_audit_enabled\":" << (analytic_audit_enabled_ ? "true" : "false")
                                 << ",\"analytic_history_default_mode\":2"
                                 << ",\"analytic_history_effective_mode\":" << volume_data_.SkyColor.w
                                 << ",\"analytic_history_default_operator\":\"lambert_diffuse_E_over_pi\""
                                 << ",\"analytic_history_effective_operator\":\""
                                 << DDGIAnalyticHistoryOperatorName(static_cast<DDGIAnalyticHistoryMode>(static_cast<u32>(volume_data_.SkyColor.w))) << '"'
                                 << ",\"material_projection_enabled\":" << (material_projection_enabled_ ? "true" : "false")
                                 << ",\"path_debug_env_present\":" << (std::getenv("DDGI_PATH_DEBUG") ? "true" : "false")
                                 << ",\"sc_enabled\":" << (sc_enabled_ ? "true" : "false")
                                 << ",\"sc_atlas_size\":" << sc_atlas_size_ << ",\"sc_lookup_count\":" << sc_lookup_count_
                                 << ",\"card_stride_bytes\":" << sizeof(SurfaceCacheCard)
                                 << ",\"card_valid_count\":null,\"card_valid_count_source\":\"bound grid-offsets header u32[3] (byte12); do not use allocation capacity\""
                                 << ",\"sc_source_identity\":\"binding0 = sc_enabled ? sc_lighting_atlas : actual prev-screen source\""
                                 << ",\"global_handle\":" << global_cb_[frameIdx]
                                 << ",\"global_is_finalize_binding\":false,\"ordinary_finalize_pipeline_handle\":" << finalize_pipeline_
                                 << ",\"bindings\":[";
                            for (u32 i = 0; i < 18; ++i)
                                meta << (i ? "," : "") << "{\"binding\":" << finalizeParams[i].binding
                                     << ",\"resource_handle\":" << finalizeParams[i].resource << '}';
                            meta << "],\"validity\":\"witness indexed by update-slot; rayData indexed by absolute probe; only update_count list prefix and fresh frame/probe/ray stamps qualify tails; unwritten capacity is not evidence\","
                                 << "\"observer_note\":\"diagnostic copies add transfer dependencies and may affect latent production synchronization races\"}\n";
                        }
                    } else {
                        DescriptorData finalizeParams[] = {
                            {3, DescriptorType::SampledImage,  lightingAtlasTex},
                            {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                            {2, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {3, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, ray_data_buffer_},
                            {5, DescriptorType::StorageBuffer, sc_card_lookup_buffer_},
                            {6, DescriptorType::StorageBuffer, sc_card_data_buffer_},
                            {7, DescriptorType::StorageBuffer, sc_grid_offsets_buffer_},
                            {8, DescriptorType::StorageBuffer, sc_grid_cards_buffer_},
                            {4, DescriptorType::SampledImage,  prevColorTex},
                            {5, DescriptorType::SampledImage,  gbufferAlbedoTex},
                            {9, DescriptorType::UniformBuffer, finalize_vp_cb_[frameIdx]},
                            {10, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                            {6, DescriptorType::SampledImage,  sc_depth_atlas_},   // T3.4 (D3)
                            {11, DescriptorType::StorageBuffer, hit_normal_buffer_}, // T4.3 (read)
                            {12, DescriptorType::StorageBuffer, relocated_origins_}, // P0: relocated origins (read)
                        };
                        UpdateDescriptorSet(device_, finalize_ds_[frameIdx], finalizeParams, 16);
                    }

                    const PipelineHandle finalizePipeline = useAnalyticFirstRho
                        ? analytic_first_rho_finalize_pipeline_ : finalize_pipeline_;
                    const PipelineLayoutHandle finalizeLayout = useAnalyticFirstRho
                        ? analytic_first_rho_finalize_layout_ : finalize_layout_;
                    const DescriptorSetHandle finalizeSet = useAnalyticFirstRho
                        ? analytic_first_rho_finalize_ds_[frameIdx] : finalize_ds_[frameIdx];
                    cmd->BindComputePipeline(finalizePipeline);
                    const DescriptorSetHandle sets[] = { finalizeSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, finalizeLayout, 0, 1, sets, 0, nullptr);

                    cmd->Dispatch(updateCount, 1, 1);
                    if (lumencompare::enabled && lumencompare::ordinal <= 6 &&
                        (lumencompare::ordinal == 3 || lumencompare::ordinal == 4) &&
                        rg_name_prefix_.empty() && finIsVk && !useAnalyticFirstRho &&
                        lumencompare::Selected("DDGI.SourceWitnessNear"))
                        lumencompare::Output(device_, cmd, "DDGI.SourceWitnessNear", "source-witness", hit_normal_buffer_, true);
                    if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.Finalize", rg_name_prefix_+"ray-radiance-distance", ray_data_buffer_, true);
                    if (measured_geometry_requested_ && lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.Finalize", rg_name_prefix_+"hit-normal-source", hit_normal_buffer_, true);
                    if (measured_geometry_requested_ && measured_owner_directory_ready_ && sc_lookup_count_>0u && lumencompare::enabled) {
                        lumencompare::Output(device_,cmd,"DDGI.Finalize",rg_name_prefix_+"owner-directory",sc_card_lookup_buffer_,true);
                        lumencompare::Output(device_,cmd,"DDGI.Finalize",rg_name_prefix_+"owner-cards",sc_card_data_buffer_,true);
                    }
                    if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.Finalize", rg_name_prefix_+"volume-constants", volume_cb_[frameIdx], true);
                    if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.Finalize", rg_name_prefix_+"update-list", probe_update_list_buffers_[frameIdx], true);
                    {
                        static bool splitOnce = false;
                        if (!splitOnce) {
                            splitOnce = true;
                            std::fprintf(stderr, "[DDGI-TracePath] SPLIT (TraceSDF+Finalize) dispatched\n");
                        }
                    }
                    }
                }
            } else if (sdfAvailable &&
                trace_pipeline_ != handles::INVALID_PIPELINE &&
                ray_data_buffer_ != handles::INVALID_RESOURCE) {
                // Update trace descriptor set
                // Platform-branch: Dawn uses sequential 0..9 (added GBuffer albedo
                // at binding 9 for Mode 11); Metal uses overlap (textures 0..3 +
                // buffers 0..6 with SC bindings 5/6).
                bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn ||
                               device_->GetPlatform() == rhi::RHIPlatform::Vulkan ||
                               device_->GetPlatform() == rhi::RHIPlatform::D3D12);
                DescriptorData traceParams[11];
                u32 traceParamCount = 0;
                if (isDawn) {
                    // Dawn: sequential 0..9 matching WGSL
                    traceParams[0] = {0, DescriptorType::SampledImage,  sdfTextures[0]};
                    traceParams[1] = {1, DescriptorType::SampledImage,  sdfTextures[1]};
                    traceParams[2] = {2, DescriptorType::SampledImage,  sdfTextures[2]};
                    traceParams[3] = {3, DescriptorType::SampledImage,  prevColorTex};
                    traceParams[4] = {4, DescriptorType::UniformBuffer, global_cb_[frameIdx]};
                    traceParams[5] = {5, DescriptorType::UniformBuffer, volume_cb_[frameIdx]};
                    traceParams[6] = {6, DescriptorType::StorageBuffer, ray_data_buffer_};
                    traceParams[7] = {7, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]};
                    traceParams[8] = {8, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]};
                    traceParams[9] = {9, DescriptorType::SampledImage,  gbufferAlbedoTex};
                    traceParamCount = 10;
                } else {
                    // Metal: overlap texture/buffer namespaces.
                    // Binding 3 = prevColorTex (non-SC) or lighting_atlas (SC).
                    // Bindings 5/6 = SC card lookup/data (unused when sc_enabled_=false).
                    ResourceHandle lightingAtlasTex = sc_enabled_
                        ? sc_lighting_atlas_
                        : prevColorTex;

                    // Same null guard as split trace path — skip trace entirely.
                    if (lightingAtlasTex == handles::INVALID_RESOURCE) {
                        // traceParamCount stays 0; dispatch is skipped below.
                    } else {
                        traceParams[0] = {0, DescriptorType::SampledImage,  sdfTextures[0]};
                        traceParams[1] = {1, DescriptorType::SampledImage,  sdfTextures[1]};
                        traceParams[2] = {2, DescriptorType::SampledImage,  sdfTextures[2]};
                        traceParams[3] = {3, DescriptorType::SampledImage,  lightingAtlasTex};
                        traceParams[4] = {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]};
                        traceParams[5] = {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]};
                        traceParams[6] = {2, DescriptorType::StorageBuffer, ray_data_buffer_};
                        traceParams[7] = {3, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]};
                        traceParams[8] = {4, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]};
                        traceParams[9] = {5, DescriptorType::StorageBuffer, sc_card_lookup_buffer_};
                        traceParams[10] = {6, DescriptorType::StorageBuffer, sc_card_data_buffer_};
                        traceParamCount = 11;
                    }
                }

                if (traceParamCount == 0) {
                    // Null lighting atlas or invalid binding — skip trace dispatch.
                } else {
                    UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, traceParamCount);

                    cmd->BindComputePipeline(trace_pipeline_);
                    const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);

                    u32 totalRayThreads = updateCount * params_.rays_per_probe;
                    u32 gx = (totalRayThreads + 63) / 64;

                    // One-time trace dispatch diagnostic. Confirms trace path is
                    // firing AND the GBuffer albedo binding (Mode 11) resolves to
                    // a real texture handle rather than the prevColorTex fallback.
                    if (dynamic_mode_) {
                        static bool tracedOnce = false;
                        if (!tracedOnce) {
                            bool usingGBuffer = (gbufferAlbedoTex != prevColorTex) &&
                                                (gbufferAlbedoTex != handles::INVALID_RESOURCE);
                            std::fprintf(stderr,
                                "[Mode11] Trace dispatched: frame=%u updateCount=%u rays/probe=%u gx=%u gbufferAlbedo=%s\n",
                                current_frame_index, updateCount, params_.rays_per_probe, gx,
                                usingGBuffer ? "live" : "fallback");
                            tracedOnce = true;
                        }
                    }

                    cmd->Dispatch(gx, 1, 1);
                }
            }

            // 2026-09-17 审计"测有效覆盖"：DDGI_ORIGIN_STATS=1 时一次性同步
            // 读回重定位分类分布（split trace 路径才有该缓冲；队列序保证读到
            // 本帧刚写的内容）。注意：独立提交先于本帧 render graph 执行，
            // 读到的是上一帧 trace 的结果——延迟到第 60 帧触发，保证缓冲已被
            // 稳态写入（首帧触发会读到 init 清零态）。
            if (std::getenv("DDGI_ORIGIN_STATS") &&
                relocated_origins_ != handles::INVALID_RESOURCE &&
                sdf_trace_pipeline_ != handles::INVALID_PIPELINE) {
                if (!origin_stats_dumped_ && ++origin_stats_tick_ >= 60u) {
                    origin_stats_dumped_ = true;
                    DumpRelocatedOriginStats(device_, relocated_origins_,
                                             params_, volume_data_);
                }
            }

            // Barrier: end encoder for strict synchronization
            cmd->MemoryBarrier(
                PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

            _t4 = chr::steady_clock::now();
            // ================================================================
            // Sub-pass 2: UpdateIrradiance
            // ================================================================
            if (irradiance_pipeline_ != handles::INVALID_PIPELINE &&
                irradiance_buffers_[outIdx] != handles::INVALID_RESOURCE) {
                static int s_irr_diag = 0;
                if (s_irr_diag < 3) { PRIMAL_LOG_STREAM(Error, "Render") << "[DDGI_IRR] exec " << s_irr_diag << std::endl; s_irr_diag++; }
                // Update irradiance descriptor set
                DescriptorData irradianceParams[] = {
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, ray_data_buffer_},
                    {3, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                    {4, DescriptorType::StorageBuffer, irradiance_buffers_[outIdx]},
                    {5, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, confidence_buffer_},
                    {7, DescriptorType::StorageBuffer, opensky_buffer_},
                };
                UpdateDescriptorSet(device_, irradiance_ds_[frameIdx], irradianceParams, 8);

                const ResourceHandle producerSources[9] = {global_cb_[frameIdx], volume_cb_[frameIdx],
                    ray_data_buffer_, irradiance_buffers_[histIdx], probe_update_list_buffers_[frameIdx],
                    confidence_buffer_, opensky_buffer_, relocated_origins_, hit_normal_buffer_};
                if (updateCount && DDGIShProducerAudit::Requested()) {
                    if (!sh_producer_audit_) sh_producer_audit_ = std::make_unique<DDGIShProducerAudit>(device_);
                    sh_producer_audit_->Before(cmd, current_frame_index, frameIdx, probeCountTotal,
                        params_.rays_per_probe, updateCount, producerSources, irradiance_buffers_[outIdx],
                        irradiance_pipeline_, irradiance_layout_, irradiance_ds_[frameIdx]);
                }

                cmd->BindComputePipeline(irradiance_pipeline_);
                const DescriptorSetHandle sets[] = { irradiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, irradiance_layout_, 0, 1, sets, 0, nullptr);

                // Metal's 8x8 cooperative kernel assigns one whole
                // threadgroup to one probe. Vulkan/Dawn assign one probe to
                // each invocation in a 64-thread group.
                const u32 irradianceGroups =
                    (device_->GetPlatform() == RHIPlatform::Metal)
                        ? updateCount
                        : ((updateCount + 63) / 64);
                const char* matureCapture = std::getenv("DDGI_RUNTIME_MATURE");
                const char* selectedFrames = std::getenv("LUMEN_COMPARE_FRAMES");
                const bool runtimeCaptureFrame = lumencompare::ordinal <= 6 ||
                    (matureCapture && std::strcmp(matureCapture,"1") == 0 &&
                     selectedFrames && *selectedFrames);
                if (lumencompare::enabled && runtimeCaptureFrame &&
                    lumencompare::Selected("DDGI.RuntimeBefore")) {
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"SH-current", irradianceParams[4].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"SH-history", irradianceParams[3].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"confidence", irradianceParams[6].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"global", irradianceParams[0].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"volume", irradianceParams[1].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeBefore", rg_name_prefix_+"update-list", irradianceParams[5].resource, true);
                    const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                        ("frame-" + std::to_string(lumencompare::ordinal));
                    std::filesystem::create_directories(directory);
                    std::ofstream meta(directory / ("ddgi-runtime-" +
                        (rg_name_prefix_.empty() ? std::string("near") : rg_name_prefix_) + ".json"));
                    meta << "{\"frame\":" << lumencompare::ordinal << ",\"native_frame\":" << current_frame_index
                         << ",\"descriptor_slot\":" << frameIdx << ",\"write_slot\":" << outIdx
                         << ",\"history_slot\":" << histIdx << ",\"probe_count\":" << probeCountTotal
                         << ",\"update_count\":" << updateCount << ",\"dispatch_groups\":" << irradianceGroups
                         << ",\"rays_per_probe\":" << params_.rays_per_probe
                         << ",\"current_handle\":" << irradianceParams[4].resource
                         << ",\"history_handle\":" << irradianceParams[3].resource
                         << ",\"confidence_handle\":" << irradianceParams[6].resource
                         << ",\"validity\":\"update-list prefix update_count; SH probe_count*9 vec4; confidence per-probe; unused capacity is not evidence\"}\n";
                }
                cmd->Dispatch(irradianceGroups, 1, 1);
                if (lumencompare::enabled && runtimeCaptureFrame &&
                    lumencompare::Selected("DDGI.RuntimeAfter"))
                    lumencompare::Output(device_, cmd, "DDGI.RuntimeAfter", rg_name_prefix_+"confidence", irradianceParams[6].resource, true);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.UpdateIrradiance", rg_name_prefix_+"SH-current", irradiance_buffers_[outIdx], true);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "DDGI.UpdateIrradiance", rg_name_prefix_+"SH-history", irradiance_buffers_[histIdx], true);
                if (updateCount && sh_producer_audit_)
                    sh_producer_audit_->After(cmd, frameIdx, updateCount, producerSources, irradiance_buffers_[outIdx]);
            }
            // barrier: irradiance 写完后才开始 depth update
            cmd->MemoryBarrier(
                PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            // ================================================================
            if (depth_pipeline_ != handles::INVALID_PIPELINE &&
                depth_buffers_[outIdx] != handles::INVALID_RESOURCE) {
                DescriptorData depthParams[] = {
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, ray_data_buffer_},
                    {3, DescriptorType::StorageBuffer, depth_buffers_[histIdx]},  // history
                    {4, DescriptorType::StorageBuffer, depth_buffers_[outIdx]},   // output
                    {5, DescriptorType::StorageBuffer, probe_update_list_buffers_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, hit_distance_buffer_},
                    {7, DescriptorType::StorageBuffer, depth_provenance_buffers_[histIdx]},
                    {8, DescriptorType::StorageBuffer, depth_provenance_buffers_[outIdx]},
                };
                UpdateDescriptorSet(device_, depth_ds_[frameIdx], depthParams,
                    (device_->GetPlatform() == RHIPlatform::Vulkan ||
                     device_->GetPlatform() == RHIPlatform::D3D12) ? (depth_provenance_enabled_?9u:7u) : 6u);

                if (!historyProbes.empty()) {
                    historySlice("DDGI.History.PreDepth","origin",relocated_origins_,16);
                    historySlice("DDGI.History.PreDepth","rays",ray_data_buffer_,u64(params_.rays_per_probe)*16);
                    lumencompare::Output(device_,cmd,"DDGI.History.PreDepth","global",global_cb_[frameIdx],true);
                    lumencompare::Output(device_,cmd,"DDGI.History.PreDepth","volume",volume_cb_[frameIdx],true);
                }
                // Selected-frame, near-volume producer evidence. No allocation
                // or readback on the default path; old ray slots outside the
                // actual update-list prefix are explicitly not observations.
                const char* depthWitnessSetting = std::getenv("DDGI_DEPTH_MOMENTS_WITNESS");
                const bool depthWitness = device_->GetPlatform() == RHIPlatform::Vulkan &&
                    rg_name_prefix_.empty() && lumencompare::enabled &&
                    depthWitnessSetting && std::strcmp(depthWitnessSetting, "1") == 0 &&
                    lumencompare::Selected("DDGI.DepthMomentsBefore");
                if (depthWitness) {
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "global", depthParams[0].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "volume", depthParams[1].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "rays", depthParams[2].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "history", depthParams[3].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "update-list", depthParams[5].resource, true);
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsBefore", "origins", relocated_origins_, true);
                    const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                        ("frame-" + std::to_string(lumencompare::ordinal));
                    std::filesystem::create_directories(directory);
                    std::ofstream meta(directory / "ddgi-depth-moments-near.json");
                    meta << "{\"ordinal\":" << lumencompare::ordinal << ",\"native_frame\":" << current_frame_index
                         << ",\"descriptor_slot\":" << frameIdx << ",\"history_slot\":" << histIdx
                         << ",\"write_slot\":" << outIdx << ",\"probe_count\":" << probeCountTotal
                         << ",\"update_count\":" << updateCount << ",\"rays_per_probe\":" << params_.rays_per_probe
                         << ",\"history_handle\":" << depthParams[3].resource << ",\"output_handle\":" << depthParams[4].resource
                         << ",\"origins_handle\":" << relocated_origins_
                         << ",\"validity\":\"only update-list[0:update_count] produces new observations; rays and depth use mapped probe index; history origin lineage not certified\"}\n";
                }

                cmd->BindComputePipeline(depth_pipeline_);
                const DescriptorSetHandle sets[] = { depth_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, depth_layout_, 0, 1, sets, 0, nullptr);

                const u32 depthGroups =
                    (device_->GetPlatform() == RHIPlatform::Metal)
                        ? updateCount
                        : ((updateCount + 63) / 64);
                // T1.5 (R4): DispatchIndirect smoke path — same group counts
                // uploaded per frame into probe_dispatch_count_buffer_.
                // Opt-in so the default dispatch stays unchanged.
                static const bool useIndirectDispatch =
                    std::getenv("PRIMAL_DDGI_INDIRECT") != nullptr;
                if (!farPolicy && useIndirectDispatch &&
                    probe_dispatch_count_buffer_ != handles::INVALID_RESOURCE) {
                    cmd->DispatchIndirect(probe_dispatch_count_buffer_, 0);
                } else {
                    cmd->Dispatch(depthGroups, 1, 1);
                }

                if (depthWitness)
                    lumencompare::Output(device_, cmd, "DDGI.DepthMomentsAfter", "output", depthParams[4].resource, true);
                if (!historyProbes.empty()) {
                    historySlice("DDGI.History.PostDepth","depth",depth_buffers_[outIdx],512);
                    historySlice("DDGI.History.PostDepth","sh",irradiance_buffers_[outIdx],144);
                }


                // T1.3 (C5): async variance readback — copy the freshly
                // written depth field into this frame's ring slot. The
                // scheduler (2 frames later) maps the slot instead of
                // Map-ing the GPU-hot depth buffer. Slot reuse safety uses
                // the same bound as the staging pool: the per-frame fence
                // waited in BeginFrame covers slot age = 3 frames.
                if (!farPolicy && depth_readback_ring_.is_initialized()) {
                    const u32 rbSlot = varianceFenced ? variance_fence_ticket_.Slot() : depth_readback_ring_.slot_for_frame(camera_data.frame_index);
                    const u64 depthBytes = (u64)probeCountTotal * 128u * sizeof(float);
                    const bool recordCopy=rbSlot<3 && (!varianceFenced || (VarianceFenceSceneStillCurrent() && depthBytes<=depth_readback_ring_.buffer_size() &&
                        variance_fence_ticket_.Record(depth_readback_ring_.buffer(rbSlot),depthBytes,depth_buffers_[outIdx],current_frame_index,
                            variance_scene_epoch_,variance_source_digest_,variance_generation_)));
                    if(recordCopy) {
                        if(varianceFenced)cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
                        cmd->CopyBuffer(depth_buffers_[outIdx],depth_readback_ring_.buffer(rbSlot),0,0,depthBytes);
                        if(varianceFenced)cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
                        ++depth_ring_copies_;
                    }
                    if(recordCopy && cpu_stall_audit::History() && cpu_stall_audit::state.active && rbSlot<3) {
                        depth_ring_recorded_frame_[rbSlot]=camera_data.frame_index;
                        depth_ring_recorded_native_[rbSlot]=cpu_stall_audit::state.frame;
                    }
                }
            }
            cmd->MemoryBarrier(
                PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

            if(cpuHistory)cpuHistory->updates=updateCount;
            last_recorded_update_count_=updateCount;
            if(farPolicy) {
                // All four production dispatches have been recorded; this is not
                // a GPU completion flag. Queue submission is owned by the caller.
                far_update_audit_.recorded=far_update_policy_.Commit(farPlan,far_update_audit_.uploaded,true);
                far_update_audit_.count=updateCount;
                far_update_audit_.rotation=far_update_policy_.Rotation();
                far_update_audit_.max_age=far_update_policy_.MaxAge();
                for(auto index:farPlan.indices) probe_states_[index].last_update_frame=current_frame_index;
            }

            // === [Mode11 Perf] report ===
            _t5 = chr::steady_clock::now();
            s_perf.samples++;
            s_perf.total_us       += _sec(T0, _t5);
            s_perf.upload_global_us += _sec(T0, _t2);   // entry → end of GlobalShaderData
            s_perf.upload_volume_us += _sec(_t2, _t3);  // VolumeData upload + priority sched
            s_perf.trace_us       += _sec(_t3, _t4);
            s_perf.irradiance_us  += _sec(_t4, _t5);    // irradiance + depth combined
            if (s_perf.samples >= 60) {
                std::fprintf(stderr,
                    "[DDGI-Perf] samples=%u avg_total=%.2fms\n"
                    "  pre+global_upload=%.2fms  volume_upload+sched=%.2fms\n"
                    "  trace_subpass=%.2fms  irrad+depth_subpass=%.2fms\n",
                    s_perf.samples,
                    s_perf.total_us / 1000.0 / s_perf.samples,
                    s_perf.upload_global_us / 1000.0 / s_perf.samples,
                    s_perf.upload_volume_us / 1000.0 / s_perf.samples,
                    s_perf.trace_us / 1000.0 / s_perf.samples,
                    s_perf.irradiance_us / 1000.0 / s_perf.samples);
                std::fflush(stderr);
                s_perf = PerfAccum{};
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
