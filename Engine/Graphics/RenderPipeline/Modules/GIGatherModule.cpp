#include "GIGatherModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/RHI/Core/RHIMath.h"
// T1.9 (E3): platform headers removed — this module is backend-agnostic
// (platform branches go through the RHI capability bits below). Do not
// re-add platform includes here; Dawn/Metal builds must stay compilable.
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <bit>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

static u64 TraversalAuditByteHash(const std::vector<u8>& bytes) {
    u64 hash=14695981039346656037ull; for(u8 byte:bytes){hash^=byte;hash*=1099511628211ull;}return hash;
}

static const char* GatherAuditFormatName(DataFormat format) {
    switch (format) {
        case DataFormat::R16_Float:    return "r16f";
        case DataFormat::RGBA16_Float: return "rgba16f";
        case DataFormat::BGRA8_UNorm:  return "bgra8";
        case DataFormat::RGBA8_UNorm:  return "rgba8";
        case DataFormat::D32_Float:    return "d32f";
        case DataFormat::R32_Float:    return "r32f";
        default:                       return "unknown";
    }
}

// r5: 变体 shader 字节装载的平台化路径。Vulkan/Metal → 原样 .spv 路径；
// D3D12 → 与 tvsrg 的 MapShaderPathForD3D12（TestVulkanSponzaRenderGraph.cpp
// :230-245）同规则：Engine/Graphics/Vulkan/shaders/ 基座换成
// EngineTest/shaders/d3d12/、尾缀 .spv → .cso（build_d3d12_shaders.py 镜像
// 布局）。CreateShader 入口名不需按平台切换：D3D12 实现忽略入口名参数
// （D3D12Shader 构造契约——DXIL 容器入口恒 main，D3D12Shader.cpp:14-18），
// Vulkan 用真实 OpEntryPoint，故装载点保持 "ddgi_gi_gather" 实参不变。
static std::string ResolveVariantShaderPath(const char* vulkanSpvPath,
                                            RHIPlatform platform) {
    std::string path = vulkanSpvPath;
    if (platform == RHIPlatform::D3D12) {
        static const char kVulkanBase[] = "Engine/Graphics/Vulkan/shaders/";
        if (path.rfind(kVulkanBase, 0) == 0)
            path = "EngineTest/shaders/d3d12/" + path.substr(sizeof(kVulkanBase) - 1);
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".spv") == 0) {
            path.resize(path.size() - 4);
            path += ".cso";
        }
    }
    return utils::ShaderRegistry::ResolveShaderPath(path);
}

struct GIGatherCB {
    math::m4x4 inv_view_projection;
    math::v4   probe_origin_spacing;
    math::v4   probe_counts;
    math::v4   camera_pos;  // T4.3: world-space camera for reflection vectors
    // T6.3 sub-A: probe_vis 数据面。debug_vis.x = GIDebug().probe_vis 模式
    // （0=Off）。经 binding 6（48B range，第三 vec4 = offset 112）上传；
    // camera_pos（offset 96）由 Vulkan binding 6 / Metal buffer(2) 读取。
    math::v4   debug_vis;
};

static_assert(offsetof(GIGatherCB, probe_origin_spacing) == 64);
static_assert(offsetof(GIGatherCB, probe_counts) == 80);
static_assert(offsetof(GIGatherCB, camera_pos) == 96);
static_assert(offsetof(GIGatherCB, debug_vis) == 112);

// T6.3 sub-B → T3.6/D1: volume 参数 CB（binding 18，std140 布局与
// DDGIGIGather.comp 的 FlatVolumeCB 一致）。NVolumeCB 原位改型：counts[].w
// 从 enabled 位改为该 volume 在 flat SSBO 里的 probe 偏移（enabled 由
// spacing>0 隐含——未用槽位 CPU 侧清零，spacing=0 被 shader 选择器跳过）。
struct FlatVolumeCB {
    math::v4 origin_spacing[4];   // xyz = origin, w = spacing
    math::v4 counts_offset[4];    // xyz = counts, w = probe offset (flat SSBO)
    math::v4 meta;                // x = 有效 volume 数（0 = flat 路径关闭）
                                  // w = low-frequency gather blend override
                                  //     [0,1], -1 keeps backend default.
};

struct GPUStaticProbeData {
    math::v4 ProbeOrigin;
    float  ProbeSpacing;
    u32    GridDimX;
    u32    GridDimY;
    u32    GridDimZ;
    float  _pad[2];
    math::v4 SkySH[9];
};

bool GIGatherModule::Initialize(RHIDeviceBase* device, ShaderHandle shader,
                                 u32 render_width, u32 render_height,
                                 ShaderHandle glossy_source_audit_shader) {
    if(device_)Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

    if (shader == handles::INVALID_SHADER) return false;

    // 15 bindings: 6 textures + 9 buffers.
    // Metal: overlapping texture/buffer namespaces (idiomatic Metal).
    // Vulkan: flat 0..14 (depth as SampledDepthImage) — matches GLSL shader.
    // D3D12 与 Vulkan 同形（flat 0..14 + .cso 镜像）；下方变体门只扩平台因子、
    // env 因子保持（变体 cso 缺失时 CreateShader 失败 → 模块 fail-closed）。
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan ||
                      device->GetPlatform() == RHIPlatform::D3D12;
    const char* glossyAuditEnv = std::getenv("DDGI_GLOSSY_SOURCE_AUDIT");
    const bool glossyAuditRequested = isVk && glossyAuditEnv &&
        glossyAuditEnv[0] == '1' && glossyAuditEnv[1] == '\0';
    const char* gatherAuditPrefix = std::getenv("DDGI_GATHER_AUDIT");
    if (glossyAuditRequested && (!gatherAuditPrefix || !gatherAuditPrefix[0])) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_GLOSSY_SOURCE_AUDIT] requested without DDGI_GATHER_AUDIT prefix"
            << std::endl;
        return false;
    }
    if (glossyAuditRequested && glossy_source_audit_shader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_GLOSSY_SOURCE_AUDIT] requested but auxiliary shader handle is invalid"
            << std::endl;
        return false;
    }
    const char* traversalEnv=std::getenv("PRIMAL_DDGI_TRAVERSAL_AUDIT");
    traversal_audit_requested_=traversalEnv && std::strcmp(traversalEnv,"1")==0;
    if(traversal_audit_requested_) {
        const char* vis=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY");
        const char* ref=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY_REFERENCE");
        const char* compact=std::getenv("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS");
        if(!isVk || !vis || std::strcmp(vis,"1")!=0 || glossyAuditRequested ||
           (ref && std::strcmp(ref,"1")==0) || (compact && std::strcmp(compact,"1")==0)) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_TRAVERSAL_AUDIT] incompatible mode" << std::endl; return false;
        }
        if(const char* frame=std::getenv("PRIMAL_DDGI_TRAVERSAL_AUDIT_FRAME")) {
            char* end=nullptr; const auto value=std::strtoul(frame,&end,10);
            if(!*frame || !end || *end || value>0xfffffffful) return false;
            traversal_audit_frame_=u32(value);
        }
    }
    glossy_source_audit_enabled_ = glossyAuditRequested;
    const char* connectionFlag=std::getenv("PRIMAL_DDGI_GEOMETRY_CONNECTIONS");
    const char* visibilityFlag=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY");
    const bool replaceVisibility=isVk && visibilityFlag && std::strcmp(visibilityFlag,"1")==0;
    const char* referenceFlag=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY_REFERENCE");
    const bool referenceVisibility=replaceVisibility && referenceFlag && std::strcmp(referenceFlag,"1")==0;
    const char* queryEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_QUERY_KERNEL");
    const bool queryEnabled=queryEnv && std::strcmp(queryEnv,"1")==0;
    const char* queryCompactEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS");
    if(queryEnabled && (!replaceVisibility || referenceVisibility || traversal_audit_requested_ || glossyAuditRequested ||
        (queryCompactEnv && std::strcmp(queryCompactEnv,"1")==0))) {
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_QUERY_KERNEL] incompatible geometry, reference, compact or observer configuration" << std::endl;
        return false;
    }
    geometry_connections_enabled_=replaceVisibility || (isVk && connectionFlag && std::strcmp(connectionFlag,"1")==0);
    if(geometry_connections_enabled_) {
        if(glossyAuditRequested) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_CONNECTIONS] source audit and connection candidate are separate variants";
            return false;
        }
        const auto path=ResolveVariantShaderPath(replaceVisibility
            ? (referenceVisibility ? "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_replace.comp.spv"
                                   : "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_anyhit.comp.spv")
            : "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry.comp.spv",
            device->GetPlatform());
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()<=0) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_CONNECTIONS] required shader missing or empty: " << path << std::endl;
            return false;
        }
        const auto count=static_cast<size_t>(file.tellg());file.seekg(0);
        std::vector<u8> bytes(count);
        if(!file.read(reinterpret_cast<char*>(bytes.data()),count)) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_CONNECTIONS] shader read failed: " << path << std::endl;
            return false;
        }
        if(traversal_audit_requested_)traversal_main_shader_hash_=TraversalAuditByteHash(bytes);
        const char* queryAudit=std::getenv("DDGI_QUERY_KERNEL_AUDIT");
        if(queryAudit && std::strcmp(queryAudit,"1")==0)query_main_shader_hash_=TraversalAuditByteHash(bytes);
        connection_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");
        if(connection_shader_==handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_CONNECTIONS] shader creation failed: " << path << std::endl;
            return false;
        }
        shader=connection_shader_;
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_CONNECTIONS] mode="
            << (replaceVisibility ? (referenceVisibility ? "geometry-replace-reference" : "geometry-anyhit") : "geometry-gate") << " shader=" << path << std::endl;
    }
    auto failAuditInitialization = [&]() {
        // A diagnostic setup failure must never leave the production pipeline
        // looking usable to a caller that ignores Initialize's bool result.
        if (glossyAuditRequested)
            pipeline_ = handles::INVALID_PIPELINE;
        return false;
    };
    // T3.6/D1: flat-buffer 路径取代 N-volume descriptor-indexing 数组与
    // near/far 双槽 legacy 路径——不再依赖 descriptor indexing，Metal 与
    // Vulkan 同一条路径（消除 nvolume_supported_ 能力差异）。
    // Retain layout data until the baseline query pipeline has actually been
    // created. The cache's shader permanently includes QUERY_KERNEL.
    std::vector<DescriptorSetLayoutBinding> normalCacheBindings,equalBoxBindings;
    bool normalCacheCompatible=false;
    if (isVk) {
        DescriptorSetLayoutBinding vkBindings[25] = {
            {0,  DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // depth
            {1,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // normal
            {2,  DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // output
            {3,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // history
            {4,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // invViewProj; camera is in binding 6
            {5,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {6,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeCounts (+debugVis, 48B)
            {7,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkySH
            {8,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkyFactor
            {9,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // staticProbeParams
            {10, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // confidenceBuffer
            {11, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // near irradiance (debug route A)
            {12, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // depthBuffer
            {13, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // gbuffer_orm (T4.3)
            {14, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // specular output (T4.3)
            // T3.6/D1: binding 17/18 原位改型——17 = flat irradiance SSBO
            //（单 buffer，offset 256 跳过头部），18 = FlatVolumeCB。原 15/16
            //（far 双槽）与 17 array[4] 删除。
            {17, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // flat irradiance (T3.6/D1)
            {18, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // FlatVolumeCB (T3.6/D1)
            // 2026-09-17 P0：近场 volume 重定位后有效起点（可见性锚点）
            {19, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // near relocated origins
            // GI 根治 Phase 3.2: flat openSky 统计（天空可见性分类）
            {20, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // flat probe openSky
        };
        if(geometry_connections_enabled_) {
            for(u32 i=0;i<4;++i) vkBindings[19+i]={23+i,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
            vkBindings[23]={27,DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr};
        }
        // R11-A2（2026-11-13，D3D12 root-signature 对齐）：本表全部
        // StorageBuffer 都是 gather shader 的只读输入（输出走绑定 2/14 的
        // storage image u2/u14），逐一置 readonly。镜像 CSO 证据
        //（dxc -dumpbin，DDGIGIGather 三变体）：storage buffer 全部声明为
        // ByteAddressBuffer t#——基础/两几何变体 t8/t10/t11/t12/t17/t19/t20，
        // 几何变体另有 t23-t26（t7 在 HLSL 文本声明、CSO 死代码剔除，同为
        // 读侧）；D3D12 ClassifyBinding 按 readonly 选 SRV/UAV 段，不置位归
        // UAV u# → 根签名缺 t 段 → CreateComputePipelineState E_INVALIDARG
        //（id=688，与 ParticlePass R11-A 同根因类）。置位放在 set_layout_
        // 创建前：equalBox/normalCache 拷贝继承；glossy 审计布局在其后
        // 改写 idx19/20 为诊断 SSBO（写侧），不受本循环影响。Vulkan 的
        // STORAGE_BUFFER 描述符类型本就覆盖读写两种访问（shader 只读 +
        // 布局 read-write 合法），本字段 Vulkan 路径零消费（见审计报告
        // §8 零影响证据）；Dawn 走下方 else 分支表，不经此表。
        for (u32 i = 0; i < 25; ++i) {
            if (vkBindings[i].descriptorType != DescriptorType::StorageBuffer)
                continue;
            vkBindings[i].readonly = true;
        }
        set_layout_ = device->CreateDescriptorSetLayout({geometry_connections_enabled_?24u:19u, vkBindings});
        const auto enabled=[](const char* name){const char* e=std::getenv(name);return e&&std::strcmp(e,"1")==0;};
        if(enabled("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX") && queryEnabled && replaceVisibility && !referenceVisibility &&
           !traversal_audit_requested_ && !glossyAuditRequested && !enabled("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") &&
           !enabled("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") && !enabled("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST") && !enabled("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE")) {
            const char* wg=std::getenv("PRIMAL_DDGI_GATHER_WORKGROUP");
            if(!wg || std::strcmp(wg,"0")==0) {equalBoxBindings.assign(vkBindings,vkBindings+24);equalBoxBindings.push_back({28,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr});equalBoxBindings.back().readonly=true;}
        }
        if(enabled("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE")) {
            normalCacheCompatible=queryEnabled&&replaceVisibility&&!referenceVisibility&&!traversal_audit_requested_&&!glossyAuditRequested&&
                !enabled("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS")&&!enabled("PRIMAL_DDGI_GEOMETRY_UPPER_BVH");
            if(normalCacheCompatible)normalCacheBindings.assign(vkBindings,vkBindings+24);
        }
        if (glossy_source_audit_enabled_) {
            // Binding 19/20 are already live production resources (relocated
            // origins/openSky).  Keep the regular 19-entry descriptor layout
            // byte-for-byte stable and append the diagnostic SSBOs only to
            // the auxiliary source layout.
            vkBindings[19] = {21, DescriptorType::StorageBuffer, 1,
                               ShaderStage::Compute, nullptr};
            vkBindings[20] = {22, DescriptorType::StorageBuffer, 1,
                               ShaderStage::Compute, nullptr};
            glossy_source_audit_set_layout_ =
                device->CreateDescriptorSetLayout({21, vkBindings});
        }
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // history
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // orm (T4.3)
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // specular output (T4.3)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // invViewProj
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // probeCounts
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // staticSkySH
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // staticSkyFactor
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // staticProbeParams
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // confidenceBuffer
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // depthBuffer
            // T3.6/D1: 10/11 原位改型为 flat SSBO + FlatVolumeCB；原 7（near
            // irr）/9（dup）/10（far irr）/11（far params）删除。
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // flat irradiance (T3.6/D1)
            {11, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // FlatVolumeCB (T3.6/D1)
            // 2026-09-17 P0：near relocated origins（Metal buffer(12)，与 Vulkan flat 19 对等）
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // near relocated origins
            // GI 根治 Phase 3.2: flat openSky（Metal buffer(13)，与 Vulkan flat 20 对等）
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // flat probe openSky
        };
        // 修复：18 个条目（含 openSky buffer(13)）但 count 传 17——最后一个
        // 条目从未进布局，Metal ddgi_gi_gather 编码期断言
        // "missing Buffer binding at index 12/13"。
        set_layout_ = device->CreateDescriptorSetLayout({18, bindings});
    }
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    if (glossy_source_audit_enabled_) {
        glossy_source_audit_layout_ =
            device->CreatePipelineLayout({1, &glossy_source_audit_set_layout_});
        if (glossy_source_audit_layout_ == handles::INVALID_PIPELINE_LAYOUT ||
            glossy_source_audit_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] failed to create auxiliary layout"
                << std::endl;
            return failAuditInitialization();
        }
    }

    ComputePipelineDesc pd{};
    pd.computeShader = shader;
    pd.layout = layout_;
    pd.threadGroupSize = {8, 8, 1};
    pipeline_ = device->CreateComputePipeline(pd);
    if (pipeline_ == handles::INVALID_PIPELINE) return false;
    if(queryEnabled) {
        const auto path=ResolveVariantShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_query_kernel.comp.spv", device->GetPlatform());
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(file && file.tellg()>0) {
            const auto count=static_cast<size_t>(file.tellg()); file.seekg(0); std::vector<u8> bytes(count);
            if(file.read(reinterpret_cast<char*>(bytes.data()),count)) {
                const char* queryAudit=std::getenv("DDGI_QUERY_KERNEL_AUDIT");
                const char* boundsAudit=std::getenv("DDGI_BOUNDS_FIRST_AUDIT");
                if((queryAudit && std::strcmp(queryAudit,"1")==0) || (boundsAudit && std::strcmp(boundsAudit,"1")==0))query_shader_hash_=TraversalAuditByteHash(bytes);
                query_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");
            }
        }
        if(query_shader_!=handles::INVALID_SHADER) {
            ComputePipelineDesc queryPd=pd; queryPd.computeShader=query_shader_;
            query_pipeline_=device->CreateComputePipeline(queryPd);
        }
        query_requested_=query_pipeline_!=handles::INVALID_PIPELINE;
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_QUERY_KERNEL] pipeline_ready=" << query_requested_ << " shader=" << path << std::endl;
    }
    if(const char* e=std::getenv("PRIMAL_DDGI_CROSS_RAY_PAIR");e && std::strcmp(e,"1")==0) {
        pair_env_requested_=true;auto exact=[](const char* n){const char* v=std::getenv(n);return v&&std::strcmp(v,"1")==0;};
        const char* wg=std::getenv("PRIMAL_DDGI_GATHER_WORKGROUP");
        const bool compatible=isVk && queryEnabled && query_pipeline_!=handles::INVALID_PIPELINE && replaceVisibility && !referenceVisibility &&
            !traversal_audit_requested_ && !glossyAuditRequested && !exact("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX") && !exact("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") &&
            !exact("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") && !exact("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST") && !exact("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE") &&
            !exact("PRIMAL_DDGI_INSTANCE_PAYLOAD_CACHE") && (!wg||std::strcmp(wg,"0")==0);
        if(compatible){const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_pair.comp.spv");
            std::ifstream f(path,std::ios::binary|std::ios::ate);if(f&&f.tellg()>0){auto count=static_cast<size_t>(f.tellg());f.seekg(0);std::vector<u8> bytes(count);
                if(f.read(reinterpret_cast<char*>(bytes.data()),count)){pair_shader_hash_=TraversalAuditByteHash(bytes);pair_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");}}
            if(pair_shader_!=handles::INVALID_SHADER){ComputePipelineDesc pp=pd;pp.computeShader=pair_shader_;pair_pipeline_=device->CreateComputePipeline(pp);}pair_requested_=pair_pipeline_!=handles::INVALID_PIPELINE;}
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_CROSS_RAY_PAIR] startup=1 compatible=" << compatible << " pipeline_ready=" << pair_requested_ << std::endl;
    }
    if(const char* e=std::getenv("PRIMAL_DDGI_INSTANCE_PAYLOAD_CACHE");e && std::strcmp(e,"1")==0) {
        payload_env_requested_=true;
        auto exact=[](const char* name){const char* v=std::getenv(name);return v && std::strcmp(v,"1")==0;};
        const char* wg=std::getenv("PRIMAL_DDGI_GATHER_WORKGROUP");
        const bool compatible=isVk && queryEnabled && query_pipeline_!=handles::INVALID_PIPELINE && replaceVisibility && !referenceVisibility &&
            !traversal_audit_requested_ && !glossyAuditRequested && !exact("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX") &&
            !exact("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") && !exact("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") &&
            !exact("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST") && !exact("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE") && (!wg || std::strcmp(wg,"0")==0);
        if(compatible) {
            const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_instance_payload.comp.spv");
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(file && file.tellg()>0){const auto count=static_cast<size_t>(file.tellg());file.seekg(0);std::vector<u8> bytes(count);
                if(file.read(reinterpret_cast<char*>(bytes.data()),count)){payload_shader_hash_=TraversalAuditByteHash(bytes);payload_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");}}
            if(payload_shader_!=handles::INVALID_SHADER){ComputePipelineDesc candidate=pd;candidate.computeShader=payload_shader_;payload_pipeline_=device->CreateComputePipeline(candidate);}
            payload_requested_=payload_pipeline_!=handles::INVALID_PIPELINE;
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_INSTANCE_PAYLOAD] startup=1 compatible=" << compatible << " pipeline_ready=" << payload_requested_ << std::endl;
    }
    if(const char* e=std::getenv("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX");e && std::strcmp(e,"1")==0) {
        equal_box_env_requested_=true;
        if(!equalBoxBindings.empty() && query_pipeline_!=handles::INVALID_PIPELINE) {
            equal_box_set_layout_=device->CreateDescriptorSetLayout({u32(equalBoxBindings.size()),equalBoxBindings.data()});
            if(equal_box_set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)equal_box_layout_=device->CreatePipelineLayout({1,&equal_box_set_layout_});
            const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_equal_box.comp.spv");
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(file && file.tellg()>0) {const auto count=static_cast<size_t>(file.tellg());file.seekg(0);std::vector<u8> bytes(count);
                if(file.read(reinterpret_cast<char*>(bytes.data()),count)){equal_box_shader_hash_=TraversalAuditByteHash(bytes);equal_box_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");}}
            if(equal_box_shader_!=handles::INVALID_SHADER && equal_box_layout_!=handles::INVALID_PIPELINE_LAYOUT) {ComputePipelineDesc ep=pd;ep.computeShader=equal_box_shader_;ep.layout=equal_box_layout_;equal_box_pipeline_=device->CreateComputePipeline(ep);}
            if(equal_box_pipeline_!=handles::INVALID_PIPELINE)for(auto& set:equal_box_sets_)set=device->CreateDescriptorSet({equal_box_set_layout_});
            equal_box_requested_=equal_box_pipeline_!=handles::INVALID_PIPELINE;
            for(auto set:equal_box_sets_)equal_box_requested_ &= set!=handles::INVALID_DESCRIPTOR_SET;
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_EQUAL_BOX] startup=1 pipeline_ready=" << equal_box_requested_ << std::endl;
    }
    const char* workgroupEnv=std::getenv("PRIMAL_DDGI_GATHER_WORKGROUP");
    const bool workgroupValid=workgroup_experiment::Parse(workgroupEnv,true,workgroup_requested_);
    if(workgroupEnv) {
        auto exact=[](const char* name) { const char* v=std::getenv(name); return v && std::strcmp(v,"1")==0; };
        const bool compatible=isVk && workgroupValid && workgroup_requested_ && queryEnabled &&
            query_pipeline_!=handles::INVALID_PIPELINE && !referenceVisibility && !traversal_audit_requested_ &&
            !glossyAuditRequested && !exact("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") &&
            !exact("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") && !exact("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE") &&
            !exact("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST");
        if(compatible) {
            const auto path=utils::ShaderRegistry::ResolveShaderPath(workgroup_requested_==32 ?
                "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_cacheless32.comp.spv" :
                "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_cacheless64.comp.spv");
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(file && file.tellg()>0) { const auto count=static_cast<size_t>(file.tellg()); file.seekg(0);std::vector<u8> bytes(count);
                if(file.read(reinterpret_cast<char*>(bytes.data()),count)) {workgroup_shader_hash_=TraversalAuditByteHash(bytes);workgroup_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");} }
            if(workgroup_shader_!=handles::INVALID_SHADER) {ComputePipelineDesc wp=pd;wp.computeShader=workgroup_shader_;wp.threadGroupSize={8,workgroup_requested_==32?4u:8u,1};workgroup_pipeline_=device->CreateComputePipeline(wp);}
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_GATHER_WORKGROUP] startup=1 requested=" << workgroup_requested_ << " valid=" << workgroupValid << " compatible=" << compatible << " pipeline_ready=" << (workgroup_pipeline_!=handles::INVALID_PIPELINE) << std::endl;
    }
    const char* boundsEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_BOUNDS_FIRST");
    if(boundsEnv && std::strcmp(boundsEnv,"1")==0) {
        auto exact=[](const char* name) { const char* value=std::getenv(name);return value && std::strcmp(value,"1")==0; };
        const bool compatible=queryEnabled && query_pipeline_!=handles::INVALID_PIPELINE && !referenceVisibility &&
            !traversal_audit_requested_ && !glossyAuditRequested && !exact("PRIMAL_DDGI_GEOMETRY_UPPER_BVH") &&
            !exact("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS") && !exact("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE");
        const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_bounds_first.comp.spv");
        if(compatible) {
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(file && file.tellg()>0) {
                const auto count=static_cast<size_t>(file.tellg()); file.seekg(0);std::vector<u8> bytes(count);
                if(file.read(reinterpret_cast<char*>(bytes.data()),count)) {
                    if(exact("DDGI_BOUNDS_FIRST_AUDIT") || exact("DDGI_QUERY_KERNEL_AUDIT"))bounds_shader_hash_=TraversalAuditByteHash(bytes);
                    bounds_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");
                }
            }
            if(bounds_shader_!=handles::INVALID_SHADER) {ComputePipelineDesc bp=pd;bp.computeShader=bounds_shader_;bounds_pipeline_=device->CreateComputePipeline(bp);}
        }
        bounds_requested_=bounds_pipeline_!=handles::INVALID_PIPELINE;
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_BOUNDS_FIRST] compatible=" << compatible << " pipeline_ready=" << bounds_requested_ << " shader=" << path << std::endl;
    }
    if(const char* cacheEnv=std::getenv("PRIMAL_DDGI_INSTANCE_NORMAL_CACHE");cacheEnv&&std::strcmp(cacheEnv,"1")==0) {
        const bool queryReady=InstanceNormalCacheQueryReady(queryEnabled,query_pipeline_!=handles::INVALID_PIPELINE);
        normal_cache_enabled_=normalCacheCompatible&&queryReady&&normal_cache_.Initialize(device,normalCacheBindings.data(),u32(normalCacheBindings.size()));
        SetInstanceNormalCacheEnabled(normal_cache_enabled_);
        if(!normal_cache_enabled_)normal_cache_.Shutdown();
        PRIMAL_LOG_STREAM(Info,"Render")<<"[DDGI_INSTANCE_NORMAL_CACHE] compatible="<<normalCacheCompatible
            <<" query_ready="<<queryReady<<" pipeline_ready="<<normal_cache_enabled_<<std::endl;
    }
    const char* upperEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_UPPER_BVH");
    upper_allowed_=replaceVisibility && !referenceVisibility && upperEnv && std::strcmp(upperEnv,"1")==0;
    upper_requested_=upper_allowed_; upper_used_=false;
    const char* compactEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS");
    const bool compactAllowed=replaceVisibility && !referenceVisibility && !upper_allowed_ && compactEnv && std::strcmp(compactEnv,"1")==0;
    compact_requested_=false; compact_used_=false;
    if(compactAllowed) {
        const char* packedEnv=std::getenv("PRIMAL_DDGI_GEOMETRY_COMPACT_PACKED");
        const bool packed=packedEnv && std::strcmp(packedEnv,"1")==0;
        const auto path=utils::ShaderRegistry::ResolveShaderPath(packed
            ? "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_compact_packed.comp.spv"
            : "Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_compact.comp.spv");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(file && file.tellg()>0) {
            const auto count=static_cast<size_t>(file.tellg()); file.seekg(0); std::vector<u8> bytes(count);
            if(file.read(reinterpret_cast<char*>(bytes.data()),count))
                compact_shader_=device->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");
        }
        if(compact_shader_!=handles::INVALID_SHADER) {
            ComputePipelineDesc compactPd=pd; compactPd.computeShader=compact_shader_;
            compact_pipeline_=device->CreateComputePipeline(compactPd);
        }
        compact_requested_=compact_pipeline_!=handles::INVALID_PIPELINE;
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_COMPACT_STACKS] pipeline_ready="
            << (compact_pipeline_!=handles::INVALID_PIPELINE) << " shader=" << path << std::endl;
    }

    if (glossy_source_audit_enabled_) {
        ComputePipelineDesc auditPd{};
        auditPd.computeShader = glossy_source_audit_shader;
        auditPd.layout = glossy_source_audit_layout_;
        auditPd.threadGroupSize = {8, 8, 1};
        glossy_source_audit_pipeline_ = device->CreateComputePipeline(auditPd);
        if (glossy_source_audit_pipeline_ == handles::INVALID_PIPELINE) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary pipeline creation failed"
                << std::endl;
            return failAuditInitialization();
        }
    }

    for (u32 i = 0; i < 3; ++i)
        descriptor_sets_[i] = device->CreateDescriptorSet({set_layout_});

    // Vulkan 全分辨率诊断直接使用各接收像素的深度/法线，避免半分辨率
    // DDGI 在线性上采样时混合不同表面。默认保留半分辨率；Metal 沿用全分辨率。
    // 命名 gi_halfres_* 沿用历史，实际尺寸以 gather_width_/gather_height_ 为准。
    const char* fullResGatherEnv = std::getenv("PRIMAL_DDGI_FULL_RES_GATHER");
    const bool fullResGatherOptIn = isVk && fullResGatherEnv &&
        std::strcmp(fullResGatherEnv, "1") == 0;
    if (device->GetPlatform() == RHIPlatform::Metal || fullResGatherOptIn) {
        gather_width_ = render_width;
        gather_height_ = render_height;
    } else {
        gather_width_ = render_width / 2;
        gather_height_ = render_height / 2;
    }
    PRIMAL_LOG_STREAM(Info, "Render") << "[DDGI_GATHER_RESOLUTION] "
        << gather_width_ << "x" << gather_height_
        << " vulkan_full_res_opt_in=" << fullResGatherOptIn << std::endl;
    TextureDesc texDesc{};
    texDesc.size = {gather_width_, gather_height_, 1};
    texDesc.format = DataFormat::RGBA16_Float;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    gi_halfres_texture_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_halfres_texture_);  // P0-B 首帧 priming
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    gi_halfres_history_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_halfres_history_);  // P0-B 首帧 priming
    // T4.3: half-res glossy specular output (RGB = SH(R) radiance, A = NdotV)
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    gi_specular_texture_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_specular_texture_);  // P0-B 首帧 priming

    if (glossy_source_audit_enabled_) {
        // Nine vec4 records per output pixel: the first three carry the
        // receiver/source tuple, the remaining six carry the exact packed
        // fetch/weight/index fields.  Keep the opt-in allocation bounded so a
        // diagnostic request cannot consume an unbounded amount of VRAM.
        const u64 pixels = static_cast<u64>(gather_width_) * gather_height_;
        constexpr u64 kRecordsPerPixel = 9;
        constexpr u64 kBytesPerRecord = 16;
        constexpr u64 kMaxBytes = 128ull * 1024ull * 1024ull;
        glossy_source_audit_bytes_ = pixels * kRecordsPerPixel * kBytesPerRecord;
        if (glossy_source_audit_bytes_ == 0 || glossy_source_audit_bytes_ > kMaxBytes) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] refusing auxiliary buffer size "
                << glossy_source_audit_bytes_ << " bytes (limit=" << kMaxBytes << ")"
                << std::endl;
            return failAuditInitialization();
        }

        TextureDesc auditTexDesc{};
        auditTexDesc.size = {gather_width_, gather_height_, 1};
        auditTexDesc.format = DataFormat::RGBA16_Float;
        auditTexDesc.type = TextureType::Texture2D;
        auditTexDesc.usage = TextureUsage::ShaderResource |
                             TextureUsage::UnorderedAccess |
                             TextureUsage::CopySource;
        auditTexDesc.name = "DDGI_GlossySourceAuditOutput";
        glossy_source_audit_output_ = device->CreateTexture(auditTexDesc);
        auditTexDesc.name = "DDGI_GlossySourceAuditSpecular";
        glossy_source_audit_specular_ = device->CreateTexture(auditTexDesc);
        if (glossy_source_audit_output_ == handles::INVALID_RESOURCE ||
            glossy_source_audit_specular_ == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary output allocation failed"
                << std::endl;
            return failAuditInitialization();
        }
        device->PrimeTextureLayout(glossy_source_audit_output_);
        device->PrimeTextureLayout(glossy_source_audit_specular_);

        BufferDesc auditDesc{};
        auditDesc.size = glossy_source_audit_bytes_;
        auditDesc.type = BufferType::Structured;
        auditDesc.usage = GPUMemoryUsage::Dynamic;
        auditDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        auditDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                              static_cast<u32>(BufferUsageFlags::TransferSrc);
        auditDesc.name = "DDGI_GlossySourceAuditRecords";
        for (u32 i = 0; i < 3; ++i) {
            glossy_source_audit_buffer_[i] = device->CreateBuffer(auditDesc);
            if (glossy_source_audit_buffer_[i] == handles::INVALID_RESOURCE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary record allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
            glossy_source_audit_sets_[i] = device->CreateDescriptorSet({glossy_source_audit_set_layout_});
            if (glossy_source_audit_sets_[i] == handles::INVALID_DESCRIPTOR_SET) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary descriptor allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
        }

        // Two vec4 records per half-res pixel: actual ORM texel/roughness/
        // frame stamp plus the exact fetched RGBA sample.  This is an
        // independent 32 MiB-per-buffer budget; the existing 9-vec4 source
        // records keep their separate 128 MiB budget above.
        constexpr u64 kMaterialRecordsPerPixel = 2;
        constexpr u64 kMaxMaterialBytes = 32ull * 1024ull * 1024ull;
        glossy_material_audit_bytes_ = pixels * kMaterialRecordsPerPixel * kBytesPerRecord;
        if (glossy_material_audit_bytes_ == 0 ||
            glossy_material_audit_bytes_ > kMaxMaterialBytes) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] refusing material buffer size "
                << glossy_material_audit_bytes_ << " bytes (limit=" << kMaxMaterialBytes << ")"
                << std::endl;
            return failAuditInitialization();
        }
        BufferDesc materialDesc{};
        materialDesc.size = glossy_material_audit_bytes_;
        materialDesc.type = BufferType::Structured;
        materialDesc.usage = GPUMemoryUsage::Dynamic;
        materialDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        materialDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                 static_cast<u32>(BufferUsageFlags::TransferSrc);
        materialDesc.name = "DDGI_GlossyMaterialAuditRecords";
        for (u32 i = 0; i < 3; ++i) {
            glossy_material_audit_buffer_[i] = device->CreateBuffer(materialDesc);
            if (glossy_material_audit_buffer_[i] == handles::INVALID_RESOURCE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] material record allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
        }
    }

    // Constant buffers — need UniformBuffer bind flag for UBO descriptors.
    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    // R7-WIN(00327): probe_cb_ 的 counts/camera_pos 子区间(64..128)需
    // 镜像到 256/512 对齐段(桌面 minUniformBufferOffsetAlignment=0x100),
    // 缓冲扩到 1024 容纳 0/256/512 三段;0 段布局逐字节不变,镜像段
    // 内容与原段相同(Mac 输出不变)。far/nvolumes/connection 仍用 256B。
    BufferDesc probeCbDesc = cbDesc;
    probeCbDesc.size = 1024;
    for (u32 i = 0; i < 3; ++i) {
        probe_cb_[i] = device->CreateBuffer(probeCbDesc);
        // T4.4: far-volume params (origin+spacing, counts).
        far_cb_[i] = device->CreateBuffer(cbDesc);
        // T6.3 sub-B: N-volume params (144B used in a 256B slot).
        nvolumes_cb_[i] = device->CreateBuffer(cbDesc);
        if(geometry_connections_enabled_) connection_cb_[i]=device->CreateBuffer(cbDesc);
    }
    if(geometry_connections_enabled_) {
        connection_unknown_cb_=device->CreateBuffer(cbDesc);
        u32 zero[64]{};
        auto* mapped=connection_unknown_cb_!=handles::INVALID_RESOURCE?device->MapBuffer(connection_unknown_cb_):nullptr;
        if(!mapped){Shutdown();pipeline_=handles::INVALID_PIPELINE;return false;}
        std::memcpy(mapped,zero,sizeof(zero));device->UnmapBuffer(connection_unknown_cb_);
        for(auto b:connection_cb_)if(b==handles::INVALID_RESOURCE){Shutdown();pipeline_=handles::INVALID_PIPELINE;return false;}
    }

    BufferDesc spCbDesc{};
    spCbDesc.size = 512;
    spCbDesc.type = BufferType::Constant;
    spCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    spCbDesc.usage = GPUMemoryUsage::Dynamic;
    spCbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    for (u32 i = 0; i < 3; ++i)
        static_probe_cb_[i] = device->CreateBuffer(spCbDesc);

    // Dummy buffer for missing static probe bindings — used as SSBO.
    BufferDesc dummyDesc{};
    dummyDesc.size = 64;
    dummyDesc.type = BufferType::Structured;
    dummyDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    dummyDesc.usage = GPUMemoryUsage::Dynamic;
    dummyDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    dummy_buffer_ = device->CreateBuffer(dummyDesc);
    if(geometry_connections_enabled_ && dummy_buffer_==handles::INVALID_RESOURCE) {
        Shutdown();pipeline_=handles::INVALID_PIPELINE;return false;
    }

    return true;
}

bool GIGatherModule::PrepareTraversalAudit(u32 nativeFrame,u32 slot) {
    using namespace traversal_audit;
    if(traversal_audit_attempted_)return false;
    traversal_audit_attempted_=true;
    const Pixel roi[]={{797,600},{798,600},{520,600},{500,600},{790,600},{800,600},{650,500},{640,500}};
    if(!BuildTraversalAuditSelection(gather_width_,gather_height_,32,18,roi,nativeFrame,slot,traversal_selection_,traversal_pixels_))return false;
    const auto path=utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_geometry_traversal_audit.comp.spv");
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file || file.tellg()<=0)return false;
    auto count=static_cast<size_t>(file.tellg());file.seekg(0);std::vector<u8> bytes(count);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),count))return false;
    traversal_shader_hash_=TraversalAuditByteHash(bytes);
    traversal_shader_=device_->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"ddgi_gi_gather");
    if(traversal_shader_==handles::INVALID_SHADER)return false;
    DescriptorSetLayoutBinding b[26]{};u32 n=0;
    const auto append=[&](u32 binding,DescriptorType type){b[n++]={binding,type,1,ShaderStage::Compute,nullptr};};
    append(0,DescriptorType::SampledDepthImage);append(1,DescriptorType::SampledImage);append(2,DescriptorType::StorageImage);append(3,DescriptorType::SampledImage);
    for(u32 i=4;i<=6;++i)append(i,DescriptorType::UniformBuffer);
    append(7,DescriptorType::StorageBuffer);append(8,DescriptorType::StorageBuffer);append(9,DescriptorType::UniformBuffer);
    for(u32 i=10;i<=12;++i)append(i,DescriptorType::StorageBuffer);
    append(13,DescriptorType::SampledImage);append(14,DescriptorType::StorageImage);append(17,DescriptorType::StorageBuffer);append(18,DescriptorType::UniformBuffer);
    append(19,DescriptorType::StorageBuffer);append(20,DescriptorType::StorageBuffer);
    for(u32 i=23;i<=26;++i)append(i,DescriptorType::StorageBuffer);
    append(27,DescriptorType::UniformBuffer);append(28,DescriptorType::UniformBuffer);append(29,DescriptorType::StorageBuffer);
    traversal_set_layout_=device_->CreateDescriptorSetLayout({n,b});
    if(traversal_set_layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    traversal_layout_=device_->CreatePipelineLayout({1,&traversal_set_layout_});
    if(traversal_layout_==handles::INVALID_PIPELINE_LAYOUT)return false;
    traversal_set_=device_->CreateDescriptorSet({traversal_set_layout_});
    if(traversal_set_==handles::INVALID_DESCRIPTOR_SET)return false;
    ComputePipelineDesc pd{};pd.computeShader=traversal_shader_;pd.layout=traversal_layout_;pd.threadGroupSize={8,8,1};
    traversal_pipeline_=device_->CreateComputePipeline(pd);if(traversal_pipeline_==handles::INVALID_PIPELINE)return false;
    BufferDesc cb{};cb.size=sizeof(Selection);cb.type=BufferType::Constant;cb.usage=cb.memoryUsage=GPUMemoryUsage::Dynamic;
    traversal_cb_=device_->CreateBuffer(cb);cb.size=u64(traversal_pixels_.size())*RecordWords*sizeof(u32);cb.type=BufferType::Structured;
    traversal_records_=device_->CreateBuffer(cb);
    if(traversal_cb_==handles::INVALID_RESOURCE || traversal_records_==handles::INVALID_RESOURCE)return false;
    auto* c=device_->MapBuffer(traversal_cb_);if(!c)return false;std::memcpy(c,&traversal_selection_,sizeof(Selection));device_->UnmapBuffer(traversal_cb_);
    auto* rec=device_->MapBuffer(traversal_records_);if(!rec)return false;std::memset(rec,0xff,size_t(cb.size));device_->UnmapBuffer(traversal_records_);
    TextureDesc t{};t.size={gather_width_,gather_height_,1};t.format=DataFormat::RGBA16_Float;t.type=TextureType::Texture2D;
    t.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;
    traversal_diffuse_=device_->CreateTexture(t);traversal_specular_=device_->CreateTexture(t);
    if(traversal_diffuse_==handles::INVALID_RESOURCE || traversal_specular_==handles::INVALID_RESOURCE)return false;
    device_->PrimeTextureLayout(traversal_diffuse_);device_->PrimeTextureLayout(traversal_specular_);
    return true;
}

void GIGatherModule::Shutdown() {
    normal_cache_.Shutdown();normal_cache_enabled_=normal_cache_requested_=normal_cache_used_=false;
    if(device_ && traversal_audit_attempted_) {
        device_->WaitIdle();
        if(traversal_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(traversal_pipeline_);
        if(traversal_set_!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(traversal_set_);
        if(traversal_layout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(traversal_layout_);
        if(traversal_set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(traversal_set_layout_);
        if(traversal_shader_!=handles::INVALID_SHADER)device_->DestroyShader(traversal_shader_);
        for(auto resource:{traversal_cb_,traversal_records_})if(resource!=handles::INVALID_RESOURCE)device_->DestroyBuffer(resource);
        for(auto resource:{traversal_diffuse_,traversal_specular_})if(resource!=handles::INVALID_RESOURCE)device_->DestroyTexture(resource);
    }
    traversal_audit_attempted_=false;traversal_audit_requested_=false;
    traversal_pipeline_=handles::INVALID_PIPELINE;traversal_set_=handles::INVALID_DESCRIPTOR_SET;
    traversal_layout_=handles::INVALID_PIPELINE_LAYOUT;traversal_set_layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    traversal_shader_=handles::INVALID_SHADER;traversal_cb_=traversal_records_=handles::INVALID_RESOURCE;
    traversal_diffuse_=traversal_specular_=handles::INVALID_RESOURCE;traversal_pixels_.clear();
    if (device_) for (auto& s : gather_audit_sources_) {
        if (s.staging!=handles::INVALID_RESOURCE) device_->DestroyBuffer(s.staging);
        s={};
    }
    if (device_) {
        for(auto& b:connection_cb_) {if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);b=handles::INVALID_RESOURCE;}
        if(connection_unknown_cb_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(connection_unknown_cb_);
        connection_unknown_cb_=handles::INVALID_RESOURCE;
        if(connection_shader_!=handles::INVALID_SHADER)device_->DestroyShader(connection_shader_);
        connection_shader_=handles::INVALID_SHADER;geometry_connections_enabled_=false;

        for (u32 i = 0; i < 3; ++i) {
            if (glossy_source_audit_sets_[i] != handles::INVALID_DESCRIPTOR_SET)
                device_->DestroyDescriptorSet(glossy_source_audit_sets_[i]);
            if (glossy_source_audit_buffer_[i] != handles::INVALID_RESOURCE)
                device_->DestroyBuffer(glossy_source_audit_buffer_[i]);
            if (glossy_material_audit_buffer_[i] != handles::INVALID_RESOURCE)
                device_->DestroyBuffer(glossy_material_audit_buffer_[i]);
            glossy_source_audit_sets_[i] = handles::INVALID_DESCRIPTOR_SET;
            glossy_source_audit_buffer_[i] = handles::INVALID_RESOURCE;
            glossy_material_audit_buffer_[i] = handles::INVALID_RESOURCE;
        }
        if (glossy_source_audit_output_ != handles::INVALID_RESOURCE)
            device_->DestroyTexture(glossy_source_audit_output_);
        if (glossy_source_audit_specular_ != handles::INVALID_RESOURCE)
            device_->DestroyTexture(glossy_source_audit_specular_);
        if (glossy_source_audit_pipeline_ != handles::INVALID_PIPELINE)
            device_->DestroyPipeline(glossy_source_audit_pipeline_);
        if (glossy_source_audit_layout_ != handles::INVALID_PIPELINE_LAYOUT)
            device_->DestroyPipelineLayout(glossy_source_audit_layout_);
        if (glossy_source_audit_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
            device_->DestroyDescriptorSetLayout(glossy_source_audit_set_layout_);
    }
    if(device_) {
        if(pair_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pair_pipeline_);
        if(pair_shader_!=handles::INVALID_SHADER)device_->DestroyShader(pair_shader_);
        pair_pipeline_=handles::INVALID_PIPELINE;pair_shader_=handles::INVALID_SHADER;pair_requested_=false;pair_env_requested_=false;pair_shader_hash_=0;pair_receipt_={};
        if(payload_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(payload_pipeline_);
        if(payload_shader_!=handles::INVALID_SHADER)device_->DestroyShader(payload_shader_);
        payload_pipeline_=handles::INVALID_PIPELINE;payload_shader_=handles::INVALID_SHADER;payload_requested_=false;payload_env_requested_=false;payload_shader_hash_=0;payload_receipt_={};
        for(auto& set:equal_box_sets_){if(set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(set);set=handles::INVALID_DESCRIPTOR_SET;}
        if(equal_box_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(equal_box_pipeline_);
        if(equal_box_shader_!=handles::INVALID_SHADER)device_->DestroyShader(equal_box_shader_);
        if(equal_box_layout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(equal_box_layout_);
        if(equal_box_set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(equal_box_set_layout_);
        equal_box_pipeline_=handles::INVALID_PIPELINE;equal_box_shader_=handles::INVALID_SHADER;equal_box_layout_=handles::INVALID_PIPELINE_LAYOUT;equal_box_set_layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;equal_box_requested_=false;equal_box_env_requested_=false;equal_box_shader_hash_=0;equal_box_receipt_={};
        if(workgroup_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(workgroup_pipeline_);
        if(workgroup_shader_!=handles::INVALID_SHADER)device_->DestroyShader(workgroup_shader_);
        workgroup_pipeline_=handles::INVALID_PIPELINE;workgroup_shader_=handles::INVALID_SHADER;workgroup_requested_=0;workgroup_shader_hash_=0;workgroup_receipt_={};
        if(bounds_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(bounds_pipeline_);
        if(bounds_shader_!=handles::INVALID_SHADER)device_->DestroyShader(bounds_shader_);
        bounds_pipeline_=handles::INVALID_PIPELINE;bounds_shader_=handles::INVALID_SHADER;
        bounds_requested_=false;bounds_used_=false;bounds_dispatch_=false;bounds_shader_hash_=0;
        if(query_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(query_pipeline_);
        if(query_shader_!=handles::INVALID_SHADER)device_->DestroyShader(query_shader_);
        query_pipeline_=handles::INVALID_PIPELINE; query_shader_=handles::INVALID_SHADER;
        query_requested_=false; query_used_=false; query_dispatch_=false;
        query_shader_hash_=0; query_main_shader_hash_=0;
        if(compact_pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(compact_pipeline_);
        if(compact_shader_!=handles::INVALID_SHADER)device_->DestroyShader(compact_shader_);
        compact_pipeline_=handles::INVALID_PIPELINE; compact_shader_=handles::INVALID_SHADER;
        compact_requested_=false; compact_used_=false;
        upper_allowed_=false;upper_requested_=false;upper_used_=false;
        for(u32 i=0;i<3;++i) {
            if(descriptor_sets_[i]!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(descriptor_sets_[i]);
            descriptor_sets_[i]=handles::INVALID_DESCRIPTOR_SET;
            for(auto* buffer:{&probe_cb_[i],&far_cb_[i],&nvolumes_cb_[i],&static_probe_cb_[i],&flat_irr_buffer_[i],&flat_opensky_buffer_[i]}) {
                if(*buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(*buffer);
                *buffer=handles::INVALID_RESOURCE;
            }
            flat_irr_capacity_[i]=flat_opensky_capacity_[i]=0;
        }
        for(auto* texture:{&gi_halfres_texture_,&gi_halfres_history_,&gi_specular_texture_}) {
            if(*texture!=handles::INVALID_RESOURCE)device_->DestroyTexture(*texture);
            *texture=handles::INVALID_RESOURCE;
        }
        for(auto* buffer:{&dummy_buffer_,&out_staging_}) {
            if(*buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(*buffer);
            *buffer=handles::INVALID_RESOURCE;
        }
        if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);
        if(layout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(layout_);
        if(set_layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(set_layout_);
    }
    pipeline_=handles::INVALID_PIPELINE;layout_=handles::INVALID_PIPELINE_LAYOUT;set_layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    out_staging_size_=0;out_dump_frame_=0xFFFFFFFFu;
    device_=nullptr;
    glossy_source_audit_output_ = handles::INVALID_RESOURCE;
    glossy_source_audit_specular_ = handles::INVALID_RESOURCE;
    glossy_source_audit_pipeline_ = handles::INVALID_PIPELINE;
    glossy_source_audit_layout_ = handles::INVALID_PIPELINE_LAYOUT;
    glossy_source_audit_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    glossy_source_audit_bytes_ = 0;
    glossy_material_audit_bytes_ = 0;
    glossy_source_audit_enabled_ = false;
    gather_audit_orm_resource_ = handles::INVALID_RESOURCE;
    gather_audit_orm_runtime_format_ = DataFormat::Unknown;
    gather_audit_orm_width_ = gather_audit_orm_height_ = 0;
    gather_audit_orm_metadata_valid_ = false;
    gather_audit_orm_from_scene_ = false;
    gather_audit_queued_=gather_audit_dumped_=false;
}

GIGatherOutputs GIGatherModule::AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs) {
    GIGatherOutputs outputs{};
    pair_receipt_={};pair_receipt_.requested=pair_env_requested_;
    payload_receipt_={};payload_receipt_.requested=payload_env_requested_;
    equal_box_receipt_={};equal_box_receipt_.requested=equal_box_env_requested_;
    workgroup_receipt_={}; workgroup_receipt_.requested=workgroup_requested_;
    compact_used_=false;upper_used_=false;query_used_=false;query_dispatch_=false;bounds_used_=false;bounds_dispatch_=false;
    if (!inputs.ddgi_pass || pipeline_ == handles::INVALID_PIPELINE) return outputs;

    // One-shot, opt-in snapshot of the actual consumer inputs, read only once
    // this completed frame slot is reused. No diagnostic mid-frame submit.
    const char* auditPrefix=std::getenv("DDGI_GATHER_AUDIT");
    if (auditPrefix && auditPrefix[0] && gather_audit_queued_ && !gather_audit_dumped_ &&
        inputs.frame_index>=gather_audit_frame_+3u &&
        inputs.current_buffer_index%3==gather_audit_slot_) {
        // Diagnostic-only queue completion certificate. The captured copies
        // were submitted on this graphics queue in the older frame. Submit a
        // checked host dependency after them, wait its fence, then map. Do not
        // infer completion merely from a reused swapchain/triple-buffer slot.
        const auto completionHandle=device_->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* completion=rhi::GetCommandBuffer(completionHandle);
        auto requireAudit=[&](bool ok,const char* operation) {
            if (!ok) {
                PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_GATHER_AUDIT] failed " << operation;
                std::abort(); // Explicit opt-in diagnostic failure; never export stale data.
            }
        };
        requireAudit(completion!=nullptr,"completion command");
        requireAudit(completion->Begin(),"Begin");
        completion->MemoryBarrier(PipelineStage::Transfer | PipelineStage::ComputeShader,
            PipelineStage::Host,AccessFlag::TransferWrite | AccessFlag::ShaderWrite,AccessFlag::HostRead);
        requireAudit(completion->End(),"End");
        requireAudit(completion->Submit(),"Submit");
        requireAudit(completion->WaitForCompletion(),"WaitForCompletion");
        device_->DestroyCommandBuffer(completionHandle);
        // The material record is only certifiable together with the exact ORM
        // imageView selected for descriptor binding 13 at the captured
        // dispatch.  A fallback is recorded explicitly so replay cannot call
        // it a scene ORM source.
        bool complete=gather_audit_orm_metadata_valid_;
        for (u32 i=0;i<gather_audit_source_count_;++i) {
            const auto& s=gather_audit_sources_[i];
            if (s.staging==handles::INVALID_RESOURCE) { complete=false;continue; }
            auto* mapped=device_->MapBuffer(s.staging,0,s.bytes);
            if (!mapped) { complete=false;continue; }
            const std::string path=std::string(auditPrefix)+"-"+s.name+".raw";
            if (FILE* out=std::fopen(path.c_str(),"wb")) {
                complete &= std::fwrite(mapped,1,s.bytes,out)==s.bytes;
                std::fclose(out);
            } else complete=false;
            device_->UnmapBuffer(s.staging);
        }
        const std::string path=std::string(auditPrefix)+"-meta.json";
        if (FILE* out=std::fopen(path.c_str(),"w")) {
            const auto glossySourceResource = glossy_source_audit_enabled_
                ? glossy_source_audit_buffer_[gather_audit_slot_]
                : handles::INVALID_RESOURCE;
            const auto materialSourceResource = glossy_source_audit_enabled_
                ? glossy_material_audit_buffer_[gather_audit_slot_]
                : handles::INVALID_RESOURCE;
            std::fprintf(out,"{\"frame\":%u,\"read_frame\":%u,\"ddgi_frame\":%u,\"ray_sample_mode\":%u,\"rays_per_probe\":%u,\"width\":%u,\"height\":%u,\"complete\":%s,\"glossy_source_audit\":{\"enabled\":%s,\"records_per_pixel\":9,\"bytes\":%llu,\"resource_handle\":%llu,\"binding\":21,\"auxiliary_scope\":\"same_inputs_shadow_dispatch\",\"default_main_preserved\":true,\"comparison_policy\":\"aux_delta_only; mismatch_unknown; never_substitute\"},\"material_source_audit\":{\"enabled\":%s,\"schema\":1,\"records_per_pixel\":2,\"bytes\":%llu,\"resource_handle\":%llu,\"binding\":22,\"layout\":\"storage_buffer\",\"format\":\"vec4x2_f32\",\"runtime_format\":%u,\"record_format\":\"vec4x2_f32\",\"record_runtime_format\":0,\"record_width\":%u,\"record_height\":%u,\"orm_width\":%u,\"orm_height\":%u,\"orm_descriptor_handle\":%llu,\"orm_binding\":13,\"orm_layout\":\"shader_resource\",\"orm_format\":\"%s\",\"orm_runtime_format\":%u,\"orm_selection\":\"%s\",\"source_identity\":\"DDGI_GLOSSY_SOURCE_AUDIT material records\",\"auxiliary_scope\":\"same_inputs_shadow_dispatch\"},\"sources\":[",
                gather_audit_frame_,inputs.frame_index,gather_audit_ddgi_frame_,gather_audit_ray_mode_,gather_audit_rays_,gather_width_,gather_height_,complete?"true":"false",
                glossy_source_audit_enabled_?"true":"false",
                static_cast<unsigned long long>(glossy_source_audit_bytes_),
                static_cast<unsigned long long>(glossySourceResource),
                materialSourceResource!=handles::INVALID_RESOURCE?"true":"false",
                static_cast<unsigned long long>(glossy_material_audit_bytes_),
                static_cast<unsigned long long>(materialSourceResource),
                0u,
                gather_width_,gather_height_,gather_audit_orm_width_,gather_audit_orm_height_,
                static_cast<unsigned long long>(gather_audit_orm_resource_),
                GatherAuditFormatName(gather_audit_orm_runtime_format_),
                static_cast<u32>(gather_audit_orm_runtime_format_),
                gather_audit_orm_from_scene_?"scene_gbuffer_orm":"fallback_descriptor");
            for (u32 i=0;i<gather_audit_source_count_;++i) {
                const auto& s=gather_audit_sources_[i];
                std::fprintf(out,"%s{\"name\":\"%s\",\"format\":\"%s\",\"runtime_format\":%u,\"bytes\":%llu,\"width\":%u,\"height\":%u,\"live\":%s,\"resource_handle\":%llu,\"binding\":%u,\"layout\":\"%s\",\"source_identity\":\"%s\"}",
                    i?",":"",s.name,s.format,s.runtime_format,static_cast<unsigned long long>(s.bytes),s.width,s.height,
                    s.staging!=handles::INVALID_RESOURCE?"true":"false",
                    static_cast<unsigned long long>(s.resource),s.binding,s.layout,s.source_identity);
            }
            std::fprintf(out,"],\"completion_checked\":true,\"begin_end_submit_checked\":true,\"explicit_host_dependency\":true,\"completion_scope\":\"checked_graphics_queue_command_after_snapshot_copies\"}\n");std::fclose(out);
        } else complete=false;
        gather_audit_dumped_=true;
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_GATHER_AUDIT] frame=" << gather_audit_frame_
            << " read_frame=" << inputs.frame_index << " complete=" << complete << std::endl;
    }

    outputs.gi_output_tex = gi_halfres_texture_;
    outputs.gi_output_rg = graph.ImportResource("DDGIHalfResGI", gi_halfres_texture_);
    outputs.gi_specular_tex = gi_specular_texture_;
    outputs.gi_specular_rg = graph.ImportResource("DDGIHalfResGlossy", gi_specular_texture_);

    // Read the slot DDGI trace + SC-DDGI integration wrote this frame
    // (currentIndex — RG ordering guarantees the writes land before this
    // read). Phase 1d: 槽位单一事实源 DDGISlotMap.h；frame_index 缺省时
    // 回退旧 cbIdx 时钟（仅未迁移的调用方触达）。
    u32 ddgiReadIdx = (inputs.frame_index != GIGatherInputs::kFrameIndexUnset)
                          ? lumen::DDGISlotsForFrame(inputs.frame_index).currentIndex
                          : ((inputs.current_buffer_index + 2) % 3);

    // Import DDGI buffers
    auto irrHandle = graph.ImportResource("DDGIIrrRead", inputs.ddgi_pass->GetIrradianceBufferBySlot(ddgiReadIdx));
    auto depthHandle = graph.ImportResource("DDGIDepthRead", inputs.ddgi_pass->GetDepthBufferBySlot(ddgiReadIdx));
    // T4.4: far volume read dependency (ordering vs. its trace pass).
    if (inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
        graph.ImportResource("DDGIFarIrrRead", inputs.ddgi_far_pass->GetIrradianceBufferBySlot(ddgiReadIdx));
    }
    // T6.3 sub-B: 附加 volume（索引 ≥2）的读依赖注册——与 far import 同模式，
    // 把第 3/4 个 volume 的 irradiance 纳入本帧图（别名注册 + 与其 trace pass
    // 的次序约束）。volume[0]/[1] 已由上方 near/far import 覆盖。
    // T3.6/D1: flat 路径不再依赖 descriptor indexing——所有有效 volume 都
    // 参与打包，import 不再受能力位门控。
    if (inputs.volume_count > 2) {
        for (u32 vi = 2; vi < inputs.volume_count && vi < GIGatherInputs::kMaxVolumes; ++vi) {
            if (inputs.volume_views[vi].IsValid())
                graph.ImportResource("DDGINvolIrrRead" + std::to_string(vi),
                                     inputs.volume_views[vi].irradiance);
        }
    }

    bool traversalDispatch=false;
    if(traversal_audit_requested_ && !traversal_audit_attempted_ && inputs.frame_index==traversal_audit_frame_ &&
       lumencompare::enabled && lumencompare::Selected("DDGI.TraversalAudit")) {
        const u32 auditSlot=inputs.current_buffer_index%3;
        if(device_->FrameSlotCompletions().CanReuse(auditSlot,0))
            traversalDispatch=PrepareTraversalAudit(inputs.frame_index,auditSlot);
        else {traversal_audit_attempted_=true;PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_TRAVERSAL_AUDIT] slot completion unavailable" << std::endl;}
        if(!traversalDispatch)PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_TRAVERSAL_AUDIT] setup failed; observer unavailable" << std::endl;
    }
    rendergraph::RGResourceHandle traversalOutRG{},traversalSpecRG{},traversalRecordsRG{},traversalCBRG{};
    if(traversalDispatch) {
        TextureDesc auditDesc{};auditDesc.size={gather_width_,gather_height_,1};
        auditDesc.format=DataFormat::RGBA16_Float;auditDesc.type=TextureType::Texture2D;
        auditDesc.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;
        traversalOutRG=graph.ImportTexture("DDGITraversalAuditDiffuse",traversal_diffuse_,auditDesc);
        traversalSpecRG=graph.ImportTexture("DDGITraversalAuditSpecular",traversal_specular_,auditDesc);
        BufferDesc d{};d.type=BufferType::Structured;d.size=u64(traversal_pixels_.size())*traversal_audit::RecordWords*sizeof(u32);
        traversalRecordsRG=graph.ImportBuffer("DDGITraversalAuditRecords",traversal_records_,d);
        d.type=BufferType::Constant;d.size=sizeof(traversal_audit::Selection);
        traversalCBRG=graph.ImportBuffer("DDGITraversalAuditSelection",traversal_cb_,d);
    }
    rendergraph::RGResourceHandle geometryRG[4]{};
    if(geometry_connections_enabled_)for(u32 i=0;i<4;++i) {
        if(inputs.geometry[i]==handles::INVALID_RESOURCE)continue;
        BufferDesc desc{};desc.type=BufferType::Structured;desc.size=inputs.geometry_sizes[i];
        geometryRG[i]=graph.ImportBuffer("DDGIConnectionGeometry"+std::to_string(i),inputs.geometry[i],desc);
    }
    rendergraph::RGResourceHandle equalBoxRG{};
    if(equal_box_requested_ && inputs.equal_box_buffer!=handles::INVALID_RESOURCE && inputs.equal_box_size) {
        BufferDesc desc{};desc.type=BufferType::Structured;desc.size=inputs.equal_box_size;
        equalBoxRG=graph.ImportBuffer("DDGIEqualBoxFlags",inputs.equal_box_buffer,desc);
    }
    rendergraph::RGResourceHandle geometryUpperRG{};
    if(upper_allowed_ && inputs.geometry_upper!=handles::INVALID_RESOURCE && inputs.geometry_upper_size) {
        BufferDesc desc{};desc.type=BufferType::Structured;desc.size=inputs.geometry_upper_size;
        geometryUpperRG=graph.ImportBuffer("DDGIConnectionGeometryUpper",inputs.geometry_upper,desc);
    }
    normal_cache_used_=false;
    const auto normalCacheRG=normal_cache_enabled_ && normal_cache_requested_
        ? normal_cache_.AddPasses(graph,inputs,geometryRG)
        : std::array<rendergraph::RGResourceHandle,3>{};
    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    // The auxiliary source shader writes only shadow outputs.  Import them
    // into the same pass so the render graph owns their UAV/SRV transitions;
    // they never replace the production gather outputs consumed downstream.
    const auto glossyAuditOutputRG = glossy_source_audit_enabled_
        ? graph.ImportResource("DDGIGlossySourceAuditOutput", glossy_source_audit_output_)
        : rendergraph::RGResourceHandle{};
    const auto glossyAuditSpecRG = glossy_source_audit_enabled_
        ? graph.ImportResource("DDGIGlossySourceAuditSpecular", glossy_source_audit_specular_)
        : rendergraph::RGResourceHandle{};

    graph.AddPass<PassData>("DDGIGIGather",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [outRG = outputs.gi_output_rg, specRG = outputs.gi_specular_rg,
         glossyAuditOutputRG, glossyAuditSpecRG, traversalOutRG, traversalSpecRG, traversalRecordsRG, traversalCBRG,
         irrHandle, depthHandle, geometryRG, geometryUpperRG, normalCacheRG, equalBoxRG,
         depthRG = inputs.gbuffer_depth_rg, normalRG = inputs.gbuffer_normal_rg, ormRG = inputs.gbuffer_orm_rg]
        (PassData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling — outputs go to imported textures
            builder.Read(irrHandle, ResourceState::ShaderResource);
            builder.Read(depthHandle, ResourceState::ShaderResource);
            for(auto h:geometryRG)if(h.IsValid())builder.Read(h,ResourceState::ShaderResource);
            if(geometryUpperRG.IsValid())builder.Read(geometryUpperRG,ResourceState::ShaderResource);
            if(equalBoxRG.IsValid())builder.Read(equalBoxRG,ResourceState::ShaderResource);
            for(auto h:normalCacheRG)if(h.IsValid())builder.Read(h,ResourceState::ShaderResource);
            if(traversalCBRG.IsValid())builder.Read(traversalCBRG,ResourceState::ShaderResource);
            if(traversalRecordsRG.IsValid())builder.Write(traversalRecordsRG,ResourceState::UnorderedAccess);
            if(traversalOutRG.IsValid())builder.Write(traversalOutRG,ResourceState::UnorderedAccess);
            if(traversalSpecRG.IsValid())builder.Write(traversalSpecRG,ResourceState::UnorderedAccess);
            if (depthRG.IsValid()) builder.Read(depthRG, ResourceState::ShaderResource);
            if (normalRG.IsValid()) builder.Read(normalRG, ResourceState::ShaderResource);
            if (ormRG.IsValid()) builder.Read(ormRG, ResourceState::ShaderResource);
            data.output = builder.Write(outRG, ResourceState::UnorderedAccess);
            builder.Write(specRG, ResourceState::UnorderedAccess);
            if (glossyAuditOutputRG.IsValid())
                builder.Write(glossyAuditOutputRG, ResourceState::UnorderedAccess);
            if (glossyAuditSpecRG.IsValid())
                builder.Write(glossyAuditSpecRG, ResourceState::UnorderedAccess);
        },
        [this, inputs, ddgiReadIdx, traversalDispatch, normalCacheRG, equalBoxRG](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            auto ddgi = inputs.ddgi_pass;
            const u32 bindingIdx = inputs.current_buffer_index % 3;
            struct AuditBinding {u64 handle=~u64(0),offset=0,range=0;}; AuditBinding auditBindings[30]{};
            const auto remember=[&](const WriteDescriptorSet* writes,u32 count) {
                if(!traversalDispatch)return;
                for(u32 i=0;i<count;++i){const auto& w=writes[i];if(w.dstBinding>=30)continue;
                    auto& b=auditBindings[w.dstBinding];if(w.imageInfo)b.handle=w.imageInfo->imageView;
                    else if(w.bufferInfo){b.handle=w.bufferInfo->buffer;b.offset=w.bufferInfo->offset;b.range=w.bufferInfo->range;}}
            };
            const DescriptorSetHandle descriptorSet = descriptor_sets_[bindingIdx];
            // Mirror the original bindings into the independent candidate set.
            // The original set/pipeline remain usable when any witness fails.
            const bool normalPrepared=normalCacheRG[0].IsValid();
            const auto updateDescriptors=[&](u32 count,const WriteDescriptorSet* writes) {
                device_->UpdateDescriptorSets(count,writes);
                if(!normalPrepared && !equalBoxRG.IsValid())return;
                std::vector<WriteDescriptorSet> copy;
                for(u32 i=0;i<count;++i)if(writes[i].dstSet==descriptorSet){
                    if(normalPrepared){copy.push_back(writes[i]);copy.back().dstSet=normal_cache_.Set(bindingIdx);}
                    if(equalBoxRG.IsValid()){copy.push_back(writes[i]);copy.back().dstSet=equal_box_sets_[bindingIdx];}
                }
                if(!copy.empty())device_->UpdateDescriptorSets(u32(copy.size()),copy.data());
            };
            const ResourceHandle probeCB = probe_cb_[bindingIdx];
            const ResourceHandle staticProbeCB = static_probe_cb_[bindingIdx];
            const ResourceHandle nvolumesCB = nvolumes_cb_[bindingIdx];
            const char* gatherPrefix = std::getenv("DDGI_GATHER_AUDIT");
            const u32 auditFrame = std::getenv("DDGI_GATHER_FRAME")
                ? u32(std::max(0, std::atoi(std::getenv("DDGI_GATHER_FRAME"))))
                : 350u;
            const bool glossyAuditDispatch = glossy_source_audit_enabled_ &&
                gatherPrefix && gatherPrefix[0] && inputs.frame_index == auditFrame;
            const DescriptorSetHandle glossyAuditSet = glossy_source_audit_sets_[bindingIdx];
            // This is populated from imgInfos[4].imageView below.  The
            // deferred material metadata must describe the descriptor that
            // actually reached the auxiliary shader, including fallback use.
            ResourceHandle actualBoundORM = handles::INVALID_RESOURCE;

            // X2/A3: 远场 volume 数据优先走 view；view 未填时回退旧 pass 指针
            // 路径。两条来源字段一致（origin/counts/spacing/irradiance 同源同
            // 槽位），CB 上传与 barrier/descriptor 取数逐字节等价。
            lumen::DDGIVolumeView farView = inputs.far_volume_view;
            if (!farView.IsValid() && inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
                farView = inputs.ddgi_far_pass->GetVolumeViewBySlot(ddgiReadIdx);
            }

            // T3.6/D1: 组装 flat volume 表。inputs.volume_views 优先（管线侧把
            // ddgi_volumes_[0..count) 全量传入）；[0]/[1] 缺省时回退 near
            // pass / farView——与旧 near/far 槽同源同槽位。flatActive 在
            // ≥1 个有效 volume 时为真（near 恒在），不再有 descriptor
            // indexing 能力门控——Metal/Vulkan 同一条 flat 路径。
            lumen::DDGIVolumeView nvolViews[GIGatherInputs::kMaxVolumes]{};
            u32 nvolCount = 0;
            {
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi) {
                    if (vi < inputs.volume_count && inputs.volume_views[vi].IsValid())
                        nvolViews[vi] = inputs.volume_views[vi];
                }
                if (!nvolViews[0].IsValid())
                    nvolViews[0] = ddgi->GetVolumeViewBySlot(ddgiReadIdx);
                if (!nvolViews[1].IsValid() && farView.IsValid())
                    nvolViews[1] = farView;
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi)
                    if (nvolViews[vi].IsValid()) nvolCount = vi + 1;
            }
            const bool flatActive = nvolCount >= 1;
            // 路径选择一次性日志（SDFViz 先例）。
            static bool loggedGatherPath = false;
            if (!loggedGatherPath) {
                loggedGatherPath = true;
                PRIMAL_LOG_STREAM(Error, "Render") << "[GIGather] path = flat-buffer (volumes="
                          << nvolCount << ")" << std::endl;
            }

            // T3.6/D1: flat 布局计算——probeOffset 累计 + 按需增长 flat SSBO。
            // [header 256B][vol0: probes×9×vec4][vol1]...；描述符绑 offset 256
            // 跳过头部，shader 侧即纯探针数组（256 满足所有后端的 SSBO
            // offset 对齐）。头部 16B 有效内容：totalProbes, volumeCount。
            u32 fvOffsets[GIGatherInputs::kMaxVolumes] = {};
            u32 fvTotalProbes = 0;
            ResourceHandle flatBuf = handles::INVALID_RESOURCE;
            if (flatActive) {
                constexpr u64 kFlatHeaderBytes = 256;
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    fvOffsets[vi] = fvTotalProbes;
                    const auto& v = nvolViews[vi];
                    fvTotalProbes += (u32)v.counts.x * (u32)v.counts.y * (u32)v.counts.z;
                }
                const u64 needed = kFlatHeaderBytes + (u64)fvTotalProbes * 144ull;
                if (flat_irr_buffer_[bindingIdx] == handles::INVALID_RESOURCE ||
                    flat_irr_capacity_[bindingIdx] < needed) {
                    // 按需增长（三槽一起换，容量一致）。实际只在首帧或
                    // volume 数变化时触发。
                    for (u32 s = 0; s < 3; ++s) {
                        if (flat_irr_buffer_[s] != handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(flat_irr_buffer_[s]);
                        BufferDesc fbDesc{};
                        fbDesc.size = needed;
                        fbDesc.type = BufferType::Structured;
                        fbDesc.usage = GPUMemoryUsage::Dynamic;
                        fbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        fbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                           static_cast<u32>(BufferUsageFlags::TransferDst);
                        fbDesc.name = "GIGather_FlatIrr";
                        flat_irr_buffer_[s] = device_->CreateBuffer(fbDesc);
                        flat_irr_capacity_[s] = needed;
                    }
                }
                flatBuf = flat_irr_buffer_[bindingIdx];
                // 头部（host 写，立即驻留；后续 CopyBuffer 只写 [256, ...)）。
                auto* hdr = static_cast<u32*>(device_->MapBuffer(flatBuf));
                if (hdr) {
                    hdr[0] = fvTotalProbes;
                    hdr[1] = nvolCount;
                    hdr[2] = 0;
                    hdr[3] = 0;
                    device_->UnmapBuffer(flatBuf);
                }
                // GI 根治 Phase 3.2 + 能量所有权 Phase 4a: flat openSky 缓冲
                // （256B 头 + probes×8B，float2/探针 = {escapeRatio, sER}），
                // 同槽位按需增长。
                const u64 neededSky = kFlatHeaderBytes + (u64)fvTotalProbes * 8ull;
                if (flat_opensky_buffer_[bindingIdx] == handles::INVALID_RESOURCE ||
                    flat_opensky_capacity_[bindingIdx] < neededSky) {
                    for (u32 s2 = 0; s2 < 3; ++s2) {
                        if (flat_opensky_buffer_[s2] != handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(flat_opensky_buffer_[s2]);
                        BufferDesc osDesc{};
                        osDesc.size = neededSky;
                        osDesc.type = BufferType::Structured;
                        osDesc.usage = GPUMemoryUsage::Dynamic;
                        osDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        osDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                           static_cast<u32>(BufferUsageFlags::TransferDst);
                        osDesc.name = "GIGather_FlatOpenSky";
                        flat_opensky_buffer_[s2] = device_->CreateBuffer(osDesc);
                        flat_opensky_capacity_[s2] = neededSky;
                    }
                }
            }

            // Upload GIGatherCB
            {
                math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
                math::m4x4 invView = Inverse(inputs.view_matrix);
                auto* cb = static_cast<GIGatherCB*>(device_->MapBuffer(probeCB));
                if (cb) {
                    cb->inv_view_projection = Inverse(vp);
                    const auto& vol = ddgi->GetVolumeData();
                    const auto& params = ddgi->GetParams();
                    cb->probe_origin_spacing = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, params.probe_spacing};
                    const u32 auditFrameStamp =
                        inputs.frame_index == GIGatherInputs::kFrameIndexUnset
                            ? 0u : inputs.frame_index + 1u;
                    cb->probe_counts = math::v4{(float)params.probe_count_x, (float)params.probe_count_y, (float)params.probe_count_z,
                        glossy_source_audit_enabled_ ? static_cast<float>(auditFrameStamp) : 9.0f};
                    cb->camera_pos = math::make_v4(math::xyz(math::column(invView, 3)), ddgi->UsesMeasuredVisibility() ? 7.0f : 1.0f);
                    // T6.3 sub-A: probe_vis 数据面——GIDebug() 热调旋钮进 CB，
                    // shader binding 6 第三 vec4（offset 112）。Off（默认）时
                    // x=0，gather 输出零变化。
                    cb->debug_vis = math::v4{static_cast<float>(
                        static_cast<u32>(lumen::GIDebug().probe_vis)), 0.0f, 0.0f, 0.0f};
                    // Opt-in scene-floor oracle, NOT a production visibility
                    // heuristic. The patch must be independently certified from
                    // loaded triangles. Only dual-policy mode5 may consume it.
                    const auto& depthPolicy=ddgi->GetVolumeData();
                    if (device_->GetPlatform()==RHIPlatform::Vulkan && depthPolicy.RaySampleMode==5 && depthPolicy.RaysPerProbe==128 &&
                        depthPolicy.SdfVoxelSizes[0].y==0.f && depthPolicy.SdfVoxelSizes[1].w==3.f) {
                        if (const char* e=std::getenv("DDGI_GATHER_FLOOR_PATCH")) {
                            float plane=0,halfX=0,halfZ=0; char trailing=0;
                            if (std::sscanf(e,"%f,%f,%f%c",&plane,&halfX,&halfZ,&trailing)==3 &&
                                std::isfinite(plane) && std::isfinite(halfX) && std::isfinite(halfZ) && halfX>0 && halfZ>0) {
                                cb->debug_vis.y=halfX; cb->debug_vis.z=halfZ; cb->debug_vis.w=plane;
                            }
                        }
                    }
                    // TEMP diagnosis: verify the CB the gather will read.
                    static bool s_cbDbg = false;
                    if (!s_cbDbg) { s_cbDbg = true;
                        PRIMAL_LOG_STREAM(Error, "Render") << "[GATHER_CB] origin_spacing=("
                                  << cb->probe_origin_spacing.x << "," << cb->probe_origin_spacing.y
                                  << "," << cb->probe_origin_spacing.z << "," << cb->probe_origin_spacing.w
                                  << ") counts=(" << cb->probe_counts.x << "," << cb->probe_counts.y
                                  << "," << cb->probe_counts.z << "," << cb->probe_counts.w
                                  << ") cam=(" << cb->camera_pos.x << "," << cb->camera_pos.y
                                  << "," << cb->camera_pos.z << ")" << std::endl; }
                    // R7-WIN(00327): 镜像段上传——把 counts(64..80)与
                    // camera_pos+debug_vis(80..128)各复制一份到 256/512
                    // 对齐段,供 binding 5/6 的对齐视图读取。0..128 段
                    // 原样保留(Mac 读原段或镜像段字节均相同,输出不变)。
                    // 必须放在全部字段写入(含上方 floor patch)之后。
                    std::memcpy(reinterpret_cast<u8*>(cb) + 256,
                                reinterpret_cast<const u8*>(cb) + 64, 16);
                    std::memcpy(reinterpret_cast<u8*>(cb) + 512,
                                reinterpret_cast<const u8*>(cb) + 80, 48);
                    device_->UnmapBuffer(probeCB);
                }
            }

            // T3.6/D1: far 双槽 CB 上传删除（far volume 数据经 flat SSBO +
            // FlatVolumeCB 到达 shader；far_cb_ 成员保留但不再绑定）。

            // GI 根治 Phase 3.2: openSky 分类开关（PRIMAL_GI_OPENSKY=0 回滚），
            // 供 FlatVolumeCB.meta.y 与下方 flat 装配共用。
            bool openskyActive = flatActive;
            if (const char* e = std::getenv("PRIMAL_GI_OPENSKY")) {
                if (e[0] == '0') openskyActive = false;
            }
            // 能量所有权 Phase 4a: openSky V2 开关（meta.z）——E_geo/E_sky
            // 拆分消费；PRIMAL_GI_OPENSKY_V2=0 回退旧整探针分类（sER=1）。
            bool openskyV2 = true;
            if (const char* e = std::getenv("PRIMAL_GI_OPENSKY_V2")) {
                if (e[0] == '0') openskyV2 = false;
            }
            // Diagnostic/quality control for the visibility-free trilinear
            // component.  Keep -1 as the default so this instrumentation does
            // not silently alter either backend's established image.
            float lowFrequencyBlend = -1.0f;
            if (const char* e = std::getenv("PRIMAL_DDGI_LOW_FREQ_BLEND")) {
                char* end = nullptr;
                const float parsed = std::strtof(e, &end);
                if (end != e && std::isfinite(parsed))
                    lowFrequencyBlend = std::clamp(parsed, 0.0f, 1.0f);
            }
            // T6.3 sub-B → T3.6/D1: upload FlatVolumeCB（binding 18）。flatActive
            // 时写入 volume[0..nvolCount) 的 origin/spacing/counts + probe
            // offset（w 槽，原 NVolumeCB 的 enabled 位改型）；否则整块清零
            // （meta.x=0 → shader 输出 dim ambient 防御路径）。无效中段槽位
            // （spacing=0）被 shader 选择器跳过。
            {
                auto* nv = static_cast<FlatVolumeCB*>(device_->MapBuffer(nvolumesCB));
                if (nv) {
                    memset(nv, 0, sizeof(FlatVolumeCB));
                    if (flatActive) {
                        for (u32 vi = 0; vi < nvolCount; ++vi) {
                            const auto& v = nvolViews[vi];
                            nv->origin_spacing[vi] = math::v4{v.origin.x, v.origin.y, v.origin.z, v.spacing};
                            nv->counts_offset[vi] = math::v4{v.counts.x, v.counts.y, v.counts.z,
                                                             (float)fvOffsets[vi]};
                        }
                        // y = openSky 分类开关（Phase 3.2，实际生效还要求
                        // openskyActive——volume 视图缺 opensky 时 shader 侧
                        // 由 dummy 兜底但开关同时关掉）。
                        // z = openSky V2 开关（Phase 4a；PRIMAL_GI_OPENSKY_V2=0
                        // 回滚旧整探针分类——shader 侧 sER=1 等价旧行为）。
                        nv->meta = math::v4{(float)nvolCount,
                                            openskyActive ? 1.0f : 0.0f,
                                            openskyV2 ? 1.0f : 0.0f,
                                            lowFrequencyBlend};
                    }
                    device_->UnmapBuffer(nvolumesCB);
                }
            }

            // Upload static probe params
            {
                auto* sp = static_cast<GPUStaticProbeData*>(device_->MapBuffer(staticProbeCB));
                if (sp) {
                    memset(sp, 0, sizeof(GPUStaticProbeData));
                    if (inputs.static_probe_volume) {
                        const auto& vol = ddgi->GetVolumeData();
                        sp->ProbeOrigin = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, 0.0f};
                        sp->ProbeSpacing = ddgi->GetParams().probe_spacing;
                    }
                    device_->UnmapBuffer(staticProbeCB);
                }
            }

            // Ensure history texture is in SRV layout (binding 3 placeholder).
            {
                ResourceBarrier histBar{};
                histBar.resource = gi_halfres_history_;
                histBar.beforeState = ResourceState::ShaderResource;
                histBar.afterState = ResourceState::ShaderResource;
                histBar.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&histBar, 1);
            }

            // Barrier DDGI buffers from UAV to ShaderResource
            {
                // T3.6/D1: 只保留仍以 SRV 直接绑定的近场 irr/depth/conf。
                // far/附加 volume 的 irradiance 改为 flat 打包的 CopyBuffer
                // 源（同 CB 内队列序先于 dispatch，无需 barrier——Vulkan 端
                // buffer barrier 本就是 no-op，Metal 无显式 buffer 布局）。
                ResourceBarrier barriers[4]{};
                int bc = 0;
                auto irrBuf = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx);
                auto depthBuf = ddgi->GetDepthBufferBySlot(ddgiReadIdx);
                auto confBuf = ddgi->GetConfidenceBuffer(ddgiReadIdx);
                // 2026-09-17 P0：近场重定位起点与 irr/depth 同为本帧 trace 产物。
                auto roBuf = nvolViews[0].IsValid() ? nvolViews[0].relocated_origins
                                                    : handles::INVALID_RESOURCE;
                if (irrBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = irrBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (depthBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = depthBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (confBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = confBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (roBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = roBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (bc > 0) cmd->InsertBarrier(barriers, bc);
            }

            // T3.6/D1: flat 打包 copy——各 volume irradiance → flat SSBO。
            // 编码于同一 CB、先于 dispatch（队列序即依赖）。bindingIdx 槽与
            // 描述符一致，头部已由上方 host 写驻留。
            ResourceHandle flatOpenSkyBuf = handles::INVALID_RESOURCE;
            if (flatActive && flatBuf != handles::INVALID_RESOURCE) {
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    const auto& v = nvolViews[vi];
                    if (v.irradiance == handles::INVALID_RESOURCE) continue;
                    const u64 probes = (u64)v.counts.x * (u64)v.counts.y * (u64)v.counts.z;
                    if (probes == 0) continue;
                    cmd->CopyBuffer(v.irradiance, flatBuf,
                                    0, 256ull + (u64)fvOffsets[vi] * 144ull,
                                    probes * 144ull);
                }
            }
            // GI 根治 Phase 3.2: flat openSky 装配（volume 顺序与 irr 一致）。
            if (openskyActive && flat_opensky_buffer_[bindingIdx] != handles::INVALID_RESOURCE) {
                flatOpenSkyBuf = flat_opensky_buffer_[bindingIdx];
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    const auto& v = nvolViews[vi];
                    if (v.opensky == handles::INVALID_RESOURCE) { openskyActive = false; break; }
                    const u64 probes = (u64)v.counts.x * (u64)v.counts.y * (u64)v.counts.z;
                    if (probes == 0) continue;
                    cmd->CopyBuffer(v.opensky, flatOpenSkyBuf,
                                    0, 256ull + (u64)fvOffsets[vi] * 8ull,
                                    probes * 8ull);
                }
            }

            // Update descriptor set
            ResourceHandle skySH = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkySH() : handles::INVALID_RESOURCE;
            ResourceHandle skyFactor = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkyFactor() : handles::INVALID_RESOURCE;

            // Texture bindings (6: depth/normal/output/history/orm/specular)
            WriteDescriptorSet texWrites[6] = {};
            DescriptorImageInfo imgInfos[6];
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan ||
                              device_->GetPlatform() == RHIPlatform::D3D12;
            const u32 ormBinding = isVk ? 13u : 4u;
            const u32 specBinding = isVk ? 14u : 5u;
            {
                // texture(0): depth (SampledDepthImage on Vulkan/Dawn, SampledImage on Metal)
                texWrites[0].dstSet = descriptorSet;
                texWrites[0].dstBinding = 0;
                texWrites[0].dstArrayElement = 0;
                texWrites[0].descriptorCount = 1;
                texWrites[0].descriptorType = isVk ? DescriptorType::SampledDepthImage : DescriptorType::SampledImage;
                texWrites[0].imageInfo = &imgInfos[0];
                imgInfos[0].sampler = handles::INVALID_SAMPLER;
                imgInfos[0].imageView = inputs.gbuffer_depth;
                imgInfos[0].imageLayout = ResourceState::ShaderResource;
                // texture(1): normal
                texWrites[1].dstSet = descriptorSet;
                texWrites[1].dstBinding = 1;
                texWrites[1].dstArrayElement = 0;
                texWrites[1].descriptorCount = 1;
                texWrites[1].descriptorType = DescriptorType::SampledImage;
                texWrites[1].imageInfo = &imgInfos[1];
                imgInfos[1].sampler = handles::INVALID_SAMPLER;
                imgInfos[1].imageView = inputs.gbuffer_normal;
                imgInfos[1].imageLayout = ResourceState::ShaderResource;
                // texture(2): output (UAV)
                texWrites[2].dstSet = descriptorSet;
                texWrites[2].dstBinding = 2;
                texWrites[2].dstArrayElement = 0;
                texWrites[2].descriptorCount = 1;
                texWrites[2].descriptorType = DescriptorType::StorageImage;
                texWrites[2].imageInfo = &imgInfos[2];
                imgInfos[2].sampler = handles::INVALID_SAMPLER;
                imgInfos[2].imageView = gi_halfres_texture_;
                imgInfos[2].imageLayout = ResourceState::UnorderedAccess;
                // texture(3): history
                texWrites[3].dstSet = descriptorSet;
                texWrites[3].dstBinding = 3;
                texWrites[3].dstArrayElement = 0;
                texWrites[3].descriptorCount = 1;
                texWrites[3].descriptorType = DescriptorType::SampledImage;
                texWrites[3].imageInfo = &imgInfos[3];
                imgInfos[3].sampler = handles::INVALID_SAMPLER;
                imgInfos[3].imageView = gi_halfres_history_; // binding 3 (unused, but must be valid)
                imgInfos[3].imageLayout = ResourceState::ShaderResource;
                // texture(4/13): gbuffer ORM (T4.3) — fallback keeps descriptor valid
                ResourceHandle ormTex = inputs.gbuffer_orm;
                if (ormTex == handles::INVALID_RESOURCE) ormTex = inputs.fallback_texture;
                if (ormTex == handles::INVALID_RESOURCE) ormTex = gi_halfres_history_;
                actualBoundORM = ormTex;
                texWrites[4].dstSet = descriptorSet;
                texWrites[4].dstBinding = ormBinding;
                texWrites[4].dstArrayElement = 0;
                texWrites[4].descriptorCount = 1;
                texWrites[4].descriptorType = DescriptorType::SampledImage;
                texWrites[4].imageInfo = &imgInfos[4];
                imgInfos[4].sampler = handles::INVALID_SAMPLER;
                imgInfos[4].imageView = ormTex;
                imgInfos[4].imageLayout = ResourceState::ShaderResource;
                // texture(5/14): glossy specular output (T4.3, UAV)
                texWrites[5].dstSet = descriptorSet;
                texWrites[5].dstBinding = specBinding;
                texWrites[5].dstArrayElement = 0;
                texWrites[5].descriptorCount = 1;
                texWrites[5].descriptorType = DescriptorType::StorageImage;
                texWrites[5].imageInfo = &imgInfos[5];
                imgInfos[5].sampler = handles::INVALID_SAMPLER;
                imgInfos[5].imageView = gi_specular_texture_;
                imgInfos[5].imageLayout = ResourceState::UnorderedAccess;
                updateDescriptors(6, texWrites);
                if(traversalDispatch) {
                    for(u32 i=0;i<6;++i)texWrites[i].dstSet=traversal_set_;
                    imgInfos[2].imageView=traversal_diffuse_;imgInfos[5].imageView=traversal_specular_;
                    remember(texWrites,6);updateDescriptors(6,texWrites);
                }
                if (glossyAuditDispatch) {
                    // Reuse the exact input image descriptors, changing only
                    // the two write targets and descriptor set.  This keeps
                    // the auxiliary pass on the same receiver data.
                    for (u32 i = 0; i < 6; ++i)
                        texWrites[i].dstSet = glossyAuditSet;
                    imgInfos[2].imageView = glossy_source_audit_output_;
                    imgInfos[5].imageView = glossy_source_audit_specular_;
                    updateDescriptors(6, texWrites);
                }
            }

            // T3.6/D1: Vulkan flat 绑定（17 = flat SSBO offset 256 跳过头部，
            // 18 = FlatVolumeCB）。原 15/16（far 双槽）与 17 array[4] 描述符
            // 写入删除。Metal 的 flat 槽（10/11）走下方 bufWrites 路径。
            const bool compareGather = lumencompare::enabled &&
                lumencompare::Selected("GIGather.Inputs");
            ResourceHandle compareFlatHandles[4]{handles::INVALID_RESOURCE, handles::INVALID_RESOURCE,
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
            if (isVk) {
                WriteDescriptorSet flatWrites[6] = {};
                DescriptorBufferInfo flatInfos[6];
                flatWrites[0].dstSet = descriptorSet;
                flatWrites[0].dstBinding = 17;
                flatWrites[0].dstArrayElement = 0;
                flatWrites[0].descriptorCount = 1;
                flatWrites[0].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[0].imageInfo = nullptr;
                flatWrites[0].bufferInfo = &flatInfos[0];
                flatInfos[0].buffer = (flatActive && flatBuf != handles::INVALID_RESOURCE)
                    ? flatBuf : dummy_buffer_;
                flatInfos[0].offset = 256;   // 跳过头部 → 纯探针数组
                flatInfos[0].range = ~0ull;
                flatWrites[1].dstSet = descriptorSet;
                flatWrites[1].dstBinding = 18;
                flatWrites[1].dstArrayElement = 0;
                flatWrites[1].descriptorCount = 1;
                flatWrites[1].descriptorType = DescriptorType::UniformBuffer;
                flatWrites[1].imageInfo = nullptr;
                flatWrites[1].bufferInfo = &flatInfos[1];
                flatInfos[1].buffer = nvolumesCB; flatInfos[1].offset = 0;
                flatInfos[1].range = sizeof(FlatVolumeCB);
                // 2026-09-17 P0：binding 19 = 近场重定位有效起点（INVALID 时绑
                // dummy，shader 侧 w=0 回退逻辑格点）。
                flatWrites[2].dstSet = descriptorSet;
                flatWrites[2].dstBinding = 19;
                flatWrites[2].dstArrayElement = 0;
                flatWrites[2].descriptorCount = 1;
                flatWrites[2].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[2].imageInfo = nullptr;
                flatWrites[2].bufferInfo = &flatInfos[2];
                {
                    ResourceHandle nearRo = nvolViews[0].IsValid()
                        ? nvolViews[0].relocated_origins : handles::INVALID_RESOURCE;
                    flatInfos[2].buffer = (nearRo != handles::INVALID_RESOURCE)
                        ? nearRo : dummy_buffer_;
                }
                flatInfos[2].offset = 0;
                flatInfos[2].range = ~0ull;
                // GI 根治 Phase 3.2: binding 20 = flat openSky（关闭时绑 dummy，
                // fvMeta.y=0 让 shader 跳过读取）。
                flatWrites[3].dstSet = descriptorSet;
                flatWrites[3].dstBinding = 20;
                flatWrites[3].dstArrayElement = 0;
                flatWrites[3].descriptorCount = 1;
                flatWrites[3].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[3].imageInfo = nullptr;
                flatWrites[3].bufferInfo = &flatInfos[3];
                flatInfos[3].buffer = (flatOpenSkyBuf != handles::INVALID_RESOURCE)
                    ? flatOpenSkyBuf : dummy_buffer_;
                // 与 flatIrradiance 同款：offset 256 跳过头部（shader 侧索引
                // 即纯探针数组）。offset 0 会让 shader 读到头部整型位模式的
                // ≈0 浮点，分类静默失效。
                flatInfos[3].offset = 256;
                flatInfos[3].range = ~0ull;
                updateDescriptors(4, flatWrites);
                if(traversalDispatch) {for(u32 i=0;i<4;++i)flatWrites[i].dstSet=traversal_set_;remember(flatWrites,4);updateDescriptors(4,flatWrites);}
                if (compareGather) {
                    const char* names[4] = {"flat-irradiance", "flat-volume", "near-origins", "flat-opensky"};
                    for (u32 i = 0; i < 4; ++i) {
                        compareFlatHandles[i] = flatInfos[i].buffer;
                        lumencompare::Output(device_, cmd, "GIGather.Inputs", names[i], flatInfos[i].buffer, true);
                    }
                }
                if (glossyAuditDispatch) {
                    for (u32 i = 0; i < 4; ++i)
                        flatWrites[i].dstSet = glossyAuditSet;
                    flatWrites[4].dstSet = glossyAuditSet;
                    flatWrites[4].dstBinding = 21;
                    flatWrites[4].dstArrayElement = 0;
                    flatWrites[4].descriptorCount = 1;
                    flatWrites[4].descriptorType = DescriptorType::StorageBuffer;
                    flatWrites[4].imageInfo = nullptr;
                    flatWrites[4].bufferInfo = &flatInfos[4];
                    flatInfos[4].buffer = glossy_source_audit_buffer_[bindingIdx];
                    flatInfos[4].offset = 0;
                    flatInfos[4].range = glossy_source_audit_bytes_;
                    flatWrites[5].dstSet = glossyAuditSet;
                    flatWrites[5].dstBinding = 22;
                    flatWrites[5].dstArrayElement = 0;
                    flatWrites[5].descriptorCount = 1;
                    flatWrites[5].descriptorType = DescriptorType::StorageBuffer;
                    flatWrites[5].imageInfo = nullptr;
                    flatWrites[5].bufferInfo = &flatInfos[5];
                    flatInfos[5].buffer = glossy_material_audit_buffer_[bindingIdx];
                    flatInfos[5].offset = 0;
                    flatInfos[5].range = glossy_material_audit_bytes_;
                    updateDescriptors(6, flatWrites);
                }
            }

            // Buffer bindings with sub-allocation from probe_cb_
            WriteDescriptorSet bufWrites[10] = {};
            DescriptorBufferInfo bufInfos[10];
            {
                for (int i = 0; i < 10; ++i) {
                    bufWrites[i].dstSet = descriptorSet;
                    bufWrites[i].dstArrayElement = 0;
                    bufWrites[i].descriptorCount = 1;
                    bufWrites[i].descriptorType = DescriptorType::UniformBuffer;
                    bufWrites[i].imageInfo = nullptr;
                    bufWrites[i].bufferInfo = &bufInfos[i];
                }
                // Vulkan: flat bindings 4..12 (texture namespace 0..3 already used).
                // Metal: overlapping buffer namespace 0..8.
                const u32 bufBase = isVk ? 4u : 0u;
                bufWrites[0].dstBinding = bufBase + 0;
                bufInfos[0].buffer = probeCB; bufInfos[0].offset = 0; bufInfos[0].range = 80;
                // R7-WIN(00327): offset 64/80 不是 minUniformBufferOffsetAlignment
                // (桌面 0x100)倍数,触发 00327。改绑上方镜像写入的 256/512 段
                // (内容与原 64..80/80..128 逐字节相同,每个 binding 视图内字节
                // 不变,Mac 输出不变);range 不变。
                bufWrites[1].dstBinding = bufBase + 1;
                bufInfos[1].buffer = probeCB; bufInfos[1].offset = 256; bufInfos[1].range = 16;  // 镜像段(原 64..80)
                bufWrites[2].dstBinding = bufBase + 2;
                bufInfos[2].buffer = probeCB; bufInfos[2].offset = 512; bufInfos[2].range = 48;  // 镜像段(原 80..128) T6.3 sub-A: +debugVis(48B)
                bufWrites[3].dstBinding = bufBase + 3; bufWrites[3].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[3].buffer = (skySH != handles::INVALID_RESOURCE) ? skySH : dummy_buffer_;
                bufInfos[3].offset = 0; bufInfos[3].range = ~0ull;
                bufWrites[4].dstBinding = bufBase + 4; bufWrites[4].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[4].buffer = (skyFactor != handles::INVALID_RESOURCE) ? skyFactor : dummy_buffer_;
                bufInfos[4].offset = 0; bufInfos[4].range = ~0ull;
                bufWrites[5].dstBinding = bufBase + 5;
                bufInfos[5].buffer = staticProbeCB; bufInfos[5].offset = 0; bufInfos[5].range = ~0ull;
                bufWrites[6].dstBinding = bufBase + 6; bufWrites[6].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[6].buffer = ddgi->GetConfidenceBuffer(ddgiReadIdx); bufInfos[6].offset = 0; bufInfos[6].range = ~0ull;
                bufWrites[7].dstBinding = bufBase + 7; bufWrites[7].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[7].buffer = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx); bufInfos[7].offset = 0; bufInfos[7].range = ~0ull;
                bufWrites[8].dstBinding = bufBase + 8; bufWrites[8].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[8].buffer = ddgi->GetDepthBufferBySlot(ddgiReadIdx); bufInfos[8].offset = 0; bufInfos[8].range = ~0ull;
                if (isVk) {
                    // Vulkan: bindings 4..12（flat 槽 17/18 已由上方 flatWrites 写入）。
                    updateDescriptors(9, bufWrites);
                    if(traversalDispatch) {for(u32 i=0;i<9;++i)bufWrites[i].dstSet=traversal_set_;remember(bufWrites,9);updateDescriptors(9,bufWrites);}
                    if (glossyAuditDispatch) {
                        for (u32 i = 0; i < 9; ++i)
                            bufWrites[i].dstSet = glossyAuditSet;
                        updateDescriptors(9, bufWrites);
                    }
                } else {
                    // T3.6/D1: Metal flat 槽（10 = flat SSBO offset 256，11 = fvCB）。
                    // 原 7（near irr）/9（dual-slot dup）/10（far irr）/11（far
                    // params）删除——dual-slot 在 Vulkan 上还会把 SSBO 写进
                    // binding 13 的 orm SampledImage 槽（基线验证错误）。
                    bufWrites[9].dstBinding = 10;
                    bufWrites[9].descriptorType = DescriptorType::StorageBuffer;
                    bufInfos[9].buffer = (flatActive && flatBuf != handles::INVALID_RESOURCE)
                        ? flatBuf : dummy_buffer_;
                    bufInfos[9].offset = 256;
                    bufInfos[9].range = ~0ull;
                    updateDescriptors(10, bufWrites);
                    // Metal fvCB（binding 11，UBO）。
                    WriteDescriptorSet fvWrite{};
                    DescriptorBufferInfo fvInfo{};
                    fvWrite.dstSet = descriptorSet;
                    fvWrite.dstBinding = 11;
                    fvWrite.dstArrayElement = 0;
                    fvWrite.descriptorCount = 1;
                    fvWrite.descriptorType = DescriptorType::UniformBuffer;
                    fvWrite.imageInfo = nullptr;
                    fvWrite.bufferInfo = &fvInfo;
                    fvInfo.buffer = nvolumesCB; fvInfo.offset = 0;
                    fvInfo.range = sizeof(FlatVolumeCB);
                    updateDescriptors(1, &fvWrite);
                    // 2026-09-17 P0：Metal buffer(12) = 近场重定位有效起点。
                    WriteDescriptorSet roWrite{};
                    DescriptorBufferInfo roInfo{};
                    roWrite.dstSet = descriptorSet;
                    roWrite.dstBinding = 12;
                    roWrite.dstArrayElement = 0;
                    roWrite.descriptorCount = 1;
                    roWrite.descriptorType = DescriptorType::StorageBuffer;
                    roWrite.imageInfo = nullptr;
                    roWrite.bufferInfo = &roInfo;
                    {
                        ResourceHandle nearRo = nvolViews[0].IsValid()
                            ? nvolViews[0].relocated_origins : handles::INVALID_RESOURCE;
                        roInfo.buffer = (nearRo != handles::INVALID_RESOURCE)
                            ? nearRo : dummy_buffer_;
                    }
                    roInfo.offset = 0;
                    roInfo.range = ~0ull;
                    updateDescriptors(1, &roWrite);   // 2026-09-15 补提交：此前填充后从未 Update，buffer(12) 缺绑 → validation abort
                    // GI 根治 Phase 3.2: Metal buffer(13) = flat openSky。
                    WriteDescriptorSet osWrite{};
                    DescriptorBufferInfo osInfo{};
                    osWrite.dstSet = descriptorSet;
                    osWrite.dstBinding = 13;
                    osWrite.dstArrayElement = 0;
                    osWrite.descriptorCount = 1;
                    osWrite.descriptorType = DescriptorType::StorageBuffer;
                    osWrite.imageInfo = nullptr;
                    osWrite.bufferInfo = &osInfo;
                    osInfo.buffer = (flatOpenSkyBuf != handles::INVALID_RESOURCE)
                        ? flatOpenSkyBuf : dummy_buffer_;
                    osInfo.offset = 0;
                    osInfo.range = ~0ull;
                    updateDescriptors(1, &osWrite);
                }
            }

            bool connectionReady=false, useUpper=false;
            math::v2 rasterJitter{};
            ResourceHandle connectionCB=connection_unknown_cb_;
            ResourceHandle connectionBuffers[4]{};
            if(geometry_connections_enabled_) {
                connectionReady=inputs.geometry_ready && bool(inputs.geometry_still_current) && inputs.geometry_still_current();
                for(u32 i=0;i<4;++i)connectionReady=connectionReady && inputs.geometry[i]!=handles::INVALID_RESOURCE && inputs.geometry_sizes[i]>0;
                connectionReady=connectionReady && bool(inputs.raster_jitter_provider) && inputs.raster_jitter_provider(rasterJitter);
                connectionReady=connectionReady && std::isfinite(rasterJitter.x) && std::isfinite(rasterJitter.y);
                struct ConnectionDispatch {u32 policy[4];float jitter[4];};
                static_assert(sizeof(ConnectionDispatch)==32);
                const ConnectionDispatch policy{{connectionReady?1u:0u,inputs.frame_index,bindingIdx,0u},{rasterJitter.x,rasterJitter.y,0.f,0.f}};
                if(auto* mapped=device_->MapBuffer(connection_cb_[bindingIdx])) {
                    std::memcpy(mapped,&policy,sizeof(policy));device_->UnmapBuffer(connection_cb_[bindingIdx]);
                    connectionCB=connection_cb_[bindingIdx];
                } else connectionReady=false;
                useUpper=connectionReady && !pair_env_requested_ && !payload_env_requested_ && upper_requested_ && upper_allowed_ &&
                    inputs.geometry_upper!=handles::INVALID_RESOURCE && inputs.geometry_upper_size>0 &&
                    inputs.geometry_upper_still_current && inputs.geometry_upper_still_current();
                DescriptorBufferInfo infos[5]{};WriteDescriptorSet writes[5]{};
                for(u32 i=0;i<5;++i) {
                    infos[i].buffer=i==4?connectionCB:(connectionReady?(useUpper && i==1?inputs.geometry_upper:inputs.geometry[i]):dummy_buffer_);
                    infos[i].offset=0;infos[i].range=i==4?32:(connectionReady?(useUpper && i==1?inputs.geometry_upper_size:inputs.geometry_sizes[i]):64);
                    writes[i].dstSet=descriptorSet;writes[i].dstBinding=23+i;writes[i].descriptorCount=1;
                    writes[i].descriptorType=i==4?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;
                    writes[i].bufferInfo=&infos[i];
                    if(i<4)connectionBuffers[i]=infos[i].buffer;
                }
                updateDescriptors(5,writes);
                if(traversalDispatch) {
                    for(auto& write:writes)write.dstSet=traversal_set_;
                    remember(writes,5);updateDescriptors(5,writes);
                    DescriptorBufferInfo extraInfos[2]{}; WriteDescriptorSet extra[2]{};
                    extraInfos[0].buffer=traversal_cb_;extraInfos[0].range=sizeof(traversal_audit::Selection);
                    extraInfos[1].buffer=traversal_records_;extraInfos[1].range=u64(traversal_pixels_.size())*traversal_audit::RecordWords*sizeof(u32);
                    for(u32 i=0;i<2;++i){extra[i].dstSet=traversal_set_;extra[i].dstBinding=28+i;extra[i].descriptorCount=1;
                        extra[i].descriptorType=i==0?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;extra[i].bufferInfo=&extraInfos[i];}
                    remember(extra,2);updateDescriptors(2,extra);
                }
                cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,
                    AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
            }
            if (compareGather && isVk) {
                if(geometry_connections_enabled_)
                    lumencompare::Output(device_,cmd,"GIGather.Inputs","connection-policy",connectionCB,true,ResourceState::ShaderResource);
                lumencompare::Output(device_, cmd, "GIGather.Inputs", "probe", bufInfos[0].buffer, true);
                lumencompare::Output(device_, cmd, "GIGather.Inputs", "near-confidence", bufInfos[6].buffer, true);
                lumencompare::Output(device_, cmd, "GIGather.Inputs", "near-SH", bufInfos[7].buffer, true);
                lumencompare::Output(device_, cmd, "GIGather.Inputs", "near-moments", bufInfos[8].buffer, true);
                const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                    ("frame-" + std::to_string(lumencompare::ordinal));
                std::filesystem::create_directories(directory);
                std::ofstream meta(directory / "gigather.json");
                meta << "{\"frame\":" << lumencompare::ordinal << ",\"native_frame\":" << inputs.frame_index
                     << ",\"binding_slot\":" << bindingIdx << ",\"read_slot\":" << ddgiReadIdx
                     << ",\"flat_active\":" << (flatActive ? "true" : "false")
                     << ",\"opensky_active\":" << (openskyActive ? "true" : "false")
                     << ",\"geometry_required\":" << (geometry_connections_enabled_?"true":"false")
                     << ",\"geometry_ready\":" << (connectionReady?"true":"false")
                     << ",\"geometry_epoch\":" << inputs.geometry_epoch
                     << ",\"geometry_source_digest\":" << inputs.geometry_source_digest
                     << ",\"geometry_handles\":[" << connectionBuffers[0] << ',' << connectionBuffers[1] << ',' << connectionBuffers[2] << ',' << connectionBuffers[3] << ']'

                     << ",\"total_probes\":" << fvTotalProbes << ",\"flat_byte_offset\":256,\"volumes\":[";
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    const auto& view = nvolViews[vi];
                    const u32 count = static_cast<u32>(view.counts.x) * static_cast<u32>(view.counts.y) * static_cast<u32>(view.counts.z);
                    meta << (vi ? "," : "") << "{\"volume\":" << vi << ",\"probe_offset\":" << fvOffsets[vi]
                         << ",\"probe_count\":" << count << ",\"source_irradiance_handle\":" << view.irradiance
                         << ",\"source_opensky_handle\":" << view.opensky << '}';
                }
                meta << "],\"flat_irradiance_handle\":" << compareFlatHandles[0]
                     << ",\"flat_volume_handle\":" << compareFlatHandles[1]
                     << ",\"near_origins_handle\":" << compareFlatHandles[2]
                     << ",\"flat_opensky_handle\":" << compareFlatHandles[3]
                     << ",\"probe_handle\":" << bufInfos[0].buffer
                     << ",\"confidence_handle\":" << bufInfos[6].buffer
                     << ",\"near_SH_handle\":" << bufInfos[7].buffer
                     << ",\"near_moments_handle\":" << bufInfos[8].buffer
                     << ",\"probe_cb_valid_bytes\":" << sizeof(GIGatherCB)
                     << ",\"validity\":\"flat irradiance at offset256 uses total_probes*144B; opensky uses total_probes*8B when active; probeCB bindings4/5/6 use byte ranges0:80/256:272(mirror of 64:80)/512:560(mirror of 80:128, R7-WIN 00327 align); unused allocation capacity is not evidence\","
                     << "\"observer_note\":\"diagnostic copies add transfer dependencies and may affect latent production synchronization races\"}\n";
            }

            // Select only after the source, jitter and upload witness have
            // been rechecked in this actual dispatch callback.
            const bool useCompact=connectionReady && !pair_env_requested_ && !payload_env_requested_ && compact_requested_ &&
                compact_pipeline_!=handles::INVALID_PIPELINE &&
                inputs.geometry_compact_still_current && inputs.geometry_compact_still_current();
            const bool useQuery=connectionReady && query_requested_ && query_pipeline_!=handles::INVALID_PIPELINE &&
                inputs.geometry_still_current && inputs.geometry_still_current() &&
                (!useUpper || (inputs.geometry_upper_still_current && inputs.geometry_upper_still_current()));
            const bool useNormals=!pair_env_requested_ && !payload_env_requested_ && normalPrepared && normal_cache_requested_ && useQuery && connectionReady && !useUpper && !useCompact && normal_cache_.Available(inputs);
            const bool useBounds=!pair_env_requested_ && !payload_env_requested_ && useQuery && !useNormals && !useUpper && !useCompact && bounds_requested_ && bounds_pipeline_!=handles::INVALID_PIPELINE;
            const bool useWorkgroup=!pair_env_requested_ && !payload_env_requested_ && useQuery && !useNormals && !useBounds && !useUpper && !useCompact && workgroup_pipeline_!=handles::INVALID_PIPELINE;
            const bool useEqualBox=!pair_env_requested_ && !payload_env_requested_ && useQuery && !useNormals && !useBounds && !useUpper && !useCompact && !useWorkgroup && equal_box_requested_ && equalBoxRG.IsValid() && inputs.equal_box_still_current && inputs.equal_box_still_current();
            if(useEqualBox) {
                DescriptorBufferInfo info{inputs.equal_box_buffer,0,inputs.equal_box_size};WriteDescriptorSet write{};write.dstSet=equal_box_sets_[bindingIdx];write.dstBinding=28;write.descriptorCount=1;write.descriptorType=DescriptorType::StorageBuffer;write.bufferInfo=&info;
                device_->UpdateDescriptorSets(1,&write);
                cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
            }
            const bool usePayload=!pair_env_requested_ && useQuery && payload_requested_ && !useNormals && !useBounds && !useUpper && !useCompact && !useWorkgroup && !useEqualBox;
            const bool usePair=useQuery && pair_requested_ && !useNormals && !useBounds && !useUpper && !useCompact && !useWorkgroup && !useEqualBox && !usePayload;
            // Diagnostic snapshot of the exact descriptor ranges read by the
            // selected reference pair dispatch. BufferSlice records failed
            // ranges as incomplete rather than exporting a dummy as geometry.
            // The observer is active only for a selected LumenCompare frame;
            // no extra commands or allocation occur on the normal path.
            if (compareGather && isVk) {
                const bool sourceCurrent = usePair && connectionReady &&
                    inputs.geometry_still_current && inputs.geometry_still_current();
                const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                    ("frame-" + std::to_string(lumencompare::ordinal));
                std::filesystem::create_directories(directory);
                std::ofstream bridge(directory / "bridge-gather-geometry.json");
                bridge << "{\"schema\":1,\"native_frame\":" << inputs.frame_index
                       << ",\"ordinal\":" << lumencompare::ordinal
                       << ",\"binding_slot\":" << bindingIdx
                       << ",\"command\":" << cmd->GetHandle()
                       << ",\"scene_epoch\":" << inputs.geometry_epoch
                       << ",\"source_digest\":" << inputs.geometry_source_digest
                       << ",\"source_current\":" << (sourceCurrent ? "true" : "false")
                       << ",\"pair_selected\":" << (usePair ? "true" : "false")
                       << ",\"completion_serial_before_record\":" << device_->FrameSlotCompletions().Serial(bindingIdx)
                       << ",\"length_semantics\":\"actual descriptor range; logical length requires separate scene witness\",\"buffers\":[";
                for (u32 i=0;i<4;++i) {
                    if (i) bridge << ',';
                    bridge << "{\"binding\":" << (23+i) << ",\"handle\":" << connectionBuffers[i]
                           << ",\"offset\":0,\"descriptor_bytes\":" << inputs.geometry_sizes[i] << '}';
                }
                bridge << "]}\n";
                if (sourceCurrent) {
                    // The geometry may have been host-uploaded this frame.
                    // Publish it directly to transfer before the observer copy.
                    cmd->MemoryBarrier(PipelineStage::Host | PipelineStage::ComputeShader,
                        PipelineStage::Transfer, AccessFlag::HostWrite | AccessFlag::ShaderWrite,
                        AccessFlag::TransferRead);
                    const char* names[4] = {"geometry-binding-23", "geometry-binding-24",
                                            "geometry-binding-25", "geometry-binding-26"};
                    for (u32 i=0;i<4;++i)
                        lumencompare::BufferSlice(device_,cmd,"GIGather.Inputs",names[i],
                            connectionBuffers[i],0,inputs.geometry_sizes[i]);
                }
            }
            cmd->BindComputePipeline(useNormals?normal_cache_.Pipeline():(useBounds?bounds_pipeline_:(useWorkgroup?workgroup_pipeline_:(useEqualBox?equal_box_pipeline_:(usePayload?payload_pipeline_:(usePair?pair_pipeline_:(useQuery ? query_pipeline_ : (useCompact ? compact_pipeline_ : pipeline_))))))));
            const DescriptorSetHandle sets[] = { useNormals?normal_cache_.Set(bindingIdx):(useEqualBox?equal_box_sets_[bindingIdx]:descriptorSet) };
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, useNormals?normal_cache_.Layout():(useEqualBox?equal_box_layout_:layout_), 0, 1, sets, 0, nullptr);
            const u32 localY=useWorkgroup && workgroup_requested_==32?4u:8u;
            const u32 groupsX=workgroup_experiment::Groups(gather_width_,8),groupsY=workgroup_experiment::Groups(gather_height_,localY);
            cmd->Dispatch(groupsX,groupsY,1);
            // Only claim qualification when the existing live query check passed;
            // other modes may be valid but do not provide this receipt proof.
            const bool receiptQualified=useQuery;
            workgroup_receipt_={workgroup_requested_,useWorkgroup?workgroup_requested_:0u,8,localY,groupsX,groupsY,gather_width_,gather_height_,bindingIdx,receiptQualified?inputs.geometry_epoch:0,receiptQualified?inputs.geometry_source_digest:0,true};
            for(u32 i=0;i<4;++i)workgroup_receipt_.buffers[i]=connectionBuffers[i];
            workgroup_receipt_.source_qualified=receiptQualified;
            pair_receipt_.actual=usePair;pair_receipt_.slot=bindingIdx;
            if(usePair){pair_receipt_.epoch=inputs.geometry_epoch;pair_receipt_.digest=inputs.geometry_source_digest;pair_receipt_.shader_hash=pair_shader_hash_;for(u32 i=0;i<4;++i)pair_receipt_.sources[i]=connectionBuffers[i];workgroup_receipt_.shader_fnv1a64=pair_shader_hash_;}
            payload_receipt_.actual=usePayload;payload_receipt_.slot=bindingIdx;
            if(usePayload){payload_receipt_.epoch=inputs.geometry_epoch;payload_receipt_.digest=inputs.geometry_source_digest;payload_receipt_.shader_hash=payload_shader_hash_;for(u32 i=0;i<4;++i)payload_receipt_.sources[i]=connectionBuffers[i];workgroup_receipt_.shader_fnv1a64=payload_shader_hash_;}
            equal_box_receipt_.actual=useEqualBox;equal_box_receipt_.slot=bindingIdx;
            if(useEqualBox){equal_box_receipt_.epoch=inputs.geometry_epoch;equal_box_receipt_.digest=inputs.geometry_source_digest;equal_box_receipt_.payload_hash=inputs.equal_box_payload_hash;equal_box_receipt_.flags_hash=inputs.equal_box_flags_hash;equal_box_receipt_.buffer=inputs.equal_box_buffer;equal_box_receipt_.bytes=inputs.equal_box_size;equal_box_receipt_.serial=inputs.equal_box_serial;for(u32 i=0;i<4;++i)equal_box_receipt_.sources[i]=connectionBuffers[i];}
            compact_used_=useCompact; upper_used_=useUpper; query_used_=useQuery && !useNormals; query_dispatch_=!useCompact && !useNormals;
            normal_cache_used_=useNormals;bounds_used_=useBounds;bounds_dispatch_=useQuery && !useNormals;
            if(useNormals) {
                normal_cache_.Receipt(inputs);
                if(compareGather && isVk) {
                    lumencompare::Output(device_,cmd,"GIGather.Inputs","instance-normal-owners",normal_cache_.Buffer(bindingIdx,0),true,ResourceState::ShaderResource);
                    lumencompare::Output(device_,cmd,"GIGather.Inputs","instance-normal-values",normal_cache_.Buffer(bindingIdx,1),true,ResourceState::ShaderResource);
                    lumencompare::Output(device_,cmd,"GIGather.Inputs","instance-normal-verified",normal_cache_.Buffer(bindingIdx,2),true,ResourceState::ShaderResource);
                }
            }

            if(traversalDispatch) {
                const bool stillCurrent=device_->FrameSlotCompletions().CanReuse(bindingIdx,0) && connectionReady && inputs.geometry_still_current && inputs.geometry_still_current() &&
                    (!useUpper || (inputs.geometry_upper_still_current && inputs.geometry_upper_still_current()));
                const auto directory=std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR"))/("frame-"+std::to_string(lumencompare::ordinal));
                std::filesystem::create_directories(directory);std::ofstream meta(directory/"ddgi-traversal-audit.json");
                meta << "{\"schema\":1,\"native_frame\":" << inputs.frame_index << ",\"ordinal\":" << lumencompare::ordinal
                     << ",\"slot\":" << bindingIdx << ",\"completion_serial\":" << device_->FrameSlotCompletions().Serial(bindingIdx) << ",\"ddgi_read_slot\":" << ddgiReadIdx << ",\"valid\":" << (stillCurrent?"true":"false")
                     << ",\"actual_upper\":" << (useUpper?"true":"false") << ",\"epoch\":" << inputs.geometry_epoch
                     << ",\"digest\":" << inputs.geometry_source_digest << ",\"width\":" << gather_width_ << ",\"height\":" << gather_height_
                     << ",\"jitter_bits\":[" << std::bit_cast<u32>(rasterJitter.x) << ',' << std::bit_cast<u32>(rasterJitter.y) << "]"
                     << ",\"source_payload_hash\":" << inputs.geometry_upper_original_hash << ",\"candidate_payload_hash\":" << inputs.geometry_upper_candidate_hash
                     << ",\"main_shader_fnv1a64\":" << traversal_main_shader_hash_ << ",\"shadow_shader_fnv1a64\":" << traversal_shader_hash_
                     << ",\"main_set\":" << descriptorSet << ",\"shadow_set\":" << traversal_set_
                     << ",\"geometry_handles\":[" << connectionBuffers[0] << ',' << connectionBuffers[1] << ',' << connectionBuffers[2] << ',' << connectionBuffers[3]
                     << "],\"original_handles\":[" << inputs.geometry[0] << ',' << inputs.geometry[1] << ',' << inputs.geometry[2] << ',' << inputs.geometry[3]
                     << "],\"original_sizes\":[" << inputs.geometry_sizes[0] << ',' << inputs.geometry_sizes[1] << ',' << inputs.geometry_sizes[2] << ',' << inputs.geometry_sizes[3]
                     << "],\"sizes\":[" << inputs.geometry_sizes[0] << ',' << (useUpper?inputs.geometry_upper_size:inputs.geometry_sizes[1]) << ',' << inputs.geometry_sizes[2] << ',' << inputs.geometry_sizes[3]
                     << "],\"shader\":\"DDGIGIGather_geometry_traversal_audit.comp.spv\",\"bindings\":[";
                bool firstBinding=true;for(u32 i=0;i<30;++i)if(auditBindings[i].handle!=~u64(0)) {
                    if(!firstBinding)meta << ',';firstBinding=false;meta << "{\"binding\":" << i << ",\"handle\":" << auditBindings[i].handle
                        << ",\"offset\":" << auditBindings[i].offset << ",\"range\":" << auditBindings[i].range << '}';
                }
                meta << "],\"selection\":[";
                for(size_t i=0;i<traversal_pixels_.size();++i){if(i)meta << ',';meta << '[' << traversal_pixels_[i].x << ',' << traversal_pixels_[i].y << ']';}meta << "]}\n";meta.close();
                if(stillCurrent) {
                    lumencompare::Output(device_,cmd,"DDGI.TraversalAudit","production-diffuse",gi_halfres_texture_,false,ResourceState::UnorderedAccess);
                    lumencompare::Output(device_,cmd,"DDGI.TraversalAudit","production-specular",gi_specular_texture_,false,ResourceState::UnorderedAccess);
                    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,
                        AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                    cmd->BindComputePipeline(traversal_pipeline_);const DescriptorSetHandle sets[]={traversal_set_};
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute,traversal_layout_,0,1,sets,0,nullptr);
                    cmd->Dispatch((gather_width_+7)/8,(gather_height_+7)/8,1);
                    lumencompare::Output(device_,cmd,"DDGI.TraversalAudit","shadow-diffuse",traversal_diffuse_,false,ResourceState::UnorderedAccess);
                    lumencompare::Output(device_,cmd,"DDGI.TraversalAudit","shadow-specular",traversal_specular_,false,ResourceState::UnorderedAccess);
                    lumencompare::Output(device_,cmd,"DDGI.TraversalAudit","records",traversal_records_,true,ResourceState::UnorderedAccess);
                } else PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_TRAVERSAL_AUDIT] stale input, observer not dispatched" << std::endl;
            }

            // Optional receiver-source pass.  It consumes the same descriptor
            // inputs and frame constants as the production dispatch, but its
            // writes go only to diagnostic shadow textures and the auxiliary
            // 9-vec4/pixel SSBO.  Production GI remains the first dispatch.
            if (glossyAuditDispatch) {
                cmd->BindComputePipeline(glossy_source_audit_pipeline_);
                const DescriptorSetHandle auditSets[] = { glossyAuditSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute,
                                        glossy_source_audit_layout_, 0, 1,
                                        auditSets, 0, nullptr);
                cmd->Dispatch((gather_width_ + 7) / 8, (gather_height_ + 7) / 8, 1);
                ResourceBarrier auditBarriers[2]{};
                auditBarriers[0].resource = glossy_source_audit_output_;
                auditBarriers[0].beforeState = ResourceState::UnorderedAccess;
                auditBarriers[0].afterState = ResourceState::ShaderResource;
                auditBarriers[0].subresource = 0xFFFFFFFF;
                auditBarriers[1].resource = glossy_source_audit_specular_;
                auditBarriers[1].beforeState = ResourceState::UnorderedAccess;
                auditBarriers[1].afterState = ResourceState::ShaderResource;
                auditBarriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(auditBarriers, 2);
            }

            // Barrier output UAV → SRV for downstream Fusion pass.
            // History blit disabled (temporal accumulation not used — causes
            // layout issues with uninitialized history texture on first frame).
            {
                ResourceBarrier outBarriers[2]{};
                outBarriers[0].resource = gi_halfres_texture_;
                outBarriers[0].beforeState = ResourceState::UnorderedAccess;
                outBarriers[0].afterState = ResourceState::ShaderResource;
                outBarriers[0].subresource = 0xFFFFFFFF;
                outBarriers[1].resource = gi_specular_texture_;
                outBarriers[1].beforeState = ResourceState::UnorderedAccess;
                outBarriers[1].afterState = ResourceState::ShaderResource;
                outBarriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(outBarriers, 2);
            }

            // Exact same-frame inputs for CPU replay/probe attribution. The
            // snapshot happens after both dispatches, while all bound buffers
            // still contain the values just consumed. No stale ray/source is
            // substituted when the auxiliary dispatch was not requested.
            if (gatherPrefix && gatherPrefix[0] && isVk && !gather_audit_queued_ &&
                inputs.frame_index==auditFrame) {
                const u64 nearProbes=u64(nvolViews[0].counts.x)*u64(nvolViews[0].counts.y)*u64(nvolViews[0].counts.z);
                gather_audit_orm_resource_ = actualBoundORM;
                gather_audit_orm_runtime_format_ = DataFormat::Unknown;
                gather_audit_orm_width_ = gather_audit_orm_height_ = 0;
                gather_audit_orm_metadata_valid_ = false;
                gather_audit_orm_from_scene_ =
                    inputs.gbuffer_orm != handles::INVALID_RESOURCE &&
                    actualBoundORM == inputs.gbuffer_orm;
                if (actualBoundORM != handles::INVALID_RESOURCE) {
                    gather_audit_orm_runtime_format_ = device_->GetTextureFormat(actualBoundORM);
                    const bool sized = device_->GetTextureSize(actualBoundORM,
                                                               gather_audit_orm_width_,
                                                               gather_audit_orm_height_);
                    gather_audit_orm_metadata_valid_ =
                        sized && gather_audit_orm_runtime_format_ != DataFormat::Unknown;
                }
                struct Source {
                    const char* name;
                    ResourceHandle resource;
                    u64 bytes;
                    u32 bpp;
                    const char* format;
                    u32 binding;
                    const char* identity;
                    const char* layout;
                };
                Source sources[15]{};
                u32 sourceCount = 0;
                bool sourceListOverflow = false;
                auto addSource = [&](const char* name, ResourceHandle resource,
                                     u64 bytes, u32 bpp, const char* format,
                                     u32 binding, const char* identity,
                                     const char* layout) {
                    if (sourceCount >= 15) {
                        sourceListOverflow = true;
                        return;
                    }
                    sources[sourceCount++] = {name, resource, bytes, bpp, format,
                                              binding, identity, layout};
                };
                addSource("view", probeCB, sizeof(GIGatherCB), 0, "f32", 6,
                          "GIGather probe/camera/debug CB", "uniform_buffer");
                addSource("volumes", nvolumesCB, sizeof(FlatVolumeCB), 0, "f32", 18,
                          "GIGather FlatVolumeCB", "uniform_buffer");
                addSource("irradiance", flatBuf, 256ull + u64(fvTotalProbes) * 144ull, 0, "f32", 17,
                          "GIGather flat irradiance", "storage_buffer");
                addSource("opensky", flatOpenSkyBuf, 256ull + u64(fvTotalProbes) * 8ull, 0, "f32", 20,
                          "GIGather flat openSky", "storage_buffer");
                addSource("moments", ddgi->GetDepthBufferBySlot(ddgiReadIdx), nearProbes * 128ull * 4ull, 0, "f32", 12,
                          "GIGather DDGI depth moments", "storage_buffer");
                addSource("origins", nvolViews[0].relocated_origins, nearProbes * 16ull, 0, "f32", 19,
                          "GIGather near relocated origins", "storage_buffer");
                addSource("depth", inputs.gbuffer_depth, 0, 4, "", 0,
                          "GIGather gbuffer depth", "shader_resource");
                addSource("normal", inputs.gbuffer_normal, 0, 8, "", 1,
                          "GIGather gbuffer normal", "shader_resource");
                addSource("output", gi_halfres_texture_, 0, 8, "", 2,
                          "GIGather production diffuse output", "shader_resource");
                const ResourceHandle provenance = ddgi->GetDepthProvenanceBufferBySlot(ddgiReadIdx);
                if (provenance != handles::INVALID_RESOURCE)
                    addSource("provenance", provenance, nearProbes * 64ull * 8ull * 4ull, 0, "u32", 0xFFFFFFFFu,
                              "DDGI depth provenance", "storage_buffer");
                addSource("spec_output", gi_specular_texture_, 0, 8, "", 14,
                          "GIGather production glossy output", "shader_resource");
                if (glossyAuditDispatch) {
                    addSource("glossy-source", glossy_source_audit_buffer_[bindingIdx],
                              glossy_source_audit_bytes_, 0, "vec4x9_f32", 21,
                              "DDGI_GLOSSY_SOURCE_AUDIT receiver records", "storage_buffer");
                    addSource("glossy-aux-output", glossy_source_audit_output_, 0, 8, "", 2,
                              "DDGI_GLOSSY_SOURCE_AUDIT auxiliary diffuse", "shader_resource");
                    addSource("glossy-aux-spec", glossy_source_audit_specular_, 0, 8, "", 14,
                              "DDGI_GLOSSY_SOURCE_AUDIT auxiliary glossy", "shader_resource");
                    addSource("glossy-material-source", glossy_material_audit_buffer_[bindingIdx],
                              glossy_material_audit_bytes_, 0, "vec4x2_f32", 22,
                              "DDGI_GLOSSY_SOURCE_AUDIT material records", "storage_buffer");
                }
                if (sourceListOverflow) {
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[DDGI_GATHER_AUDIT] source snapshot exceeds 15-entry bound"
                        << std::endl;
                    gather_audit_orm_metadata_valid_ = false;
                }
                gather_audit_source_count_=sourceCount;
                const auto auditVolume=ddgi->GetVolumeData();
                gather_audit_ddgi_frame_=auditVolume.FrameIndex;
                gather_audit_ray_mode_=u32(auditVolume.RaySampleMode);
                gather_audit_rays_=auditVolume.RaysPerProbe;
                cmd->MemoryBarrier(PipelineStage::ComputeShader | PipelineStage::Transfer | PipelineStage::Host,
                    PipelineStage::Transfer,AccessFlag::ShaderWrite | AccessFlag::TransferWrite | AccessFlag::HostWrite,
                    AccessFlag::TransferRead);
                for (u32 i=0;i<gather_audit_source_count_;++i) {
                    const auto& src=sources[i];auto& dst=gather_audit_sources_[i];
                    dst.name=src.name;dst.resource=src.resource;dst.bytes=src.bytes;
                    dst.format=src.format;dst.layout=src.layout;
                    dst.source_identity=src.identity;dst.binding=src.binding;
                    dst.runtime_format=0;
                    if (src.resource==handles::INVALID_RESOURCE) continue;
                    u32 bpp=src.bpp;
                    if (src.bpp) {
                        const auto format=device_->GetTextureFormat(src.resource);
                        dst.runtime_format=static_cast<u32>(format);
                        if (format==DataFormat::BGRA8_UNorm) { bpp=4;dst.format="bgra8"; }
                        else if (format==DataFormat::RGBA8_UNorm) { bpp=4;dst.format="rgba8"; }
                        else if (format==DataFormat::RGBA16_Float) { bpp=8;dst.format="rgba16f"; }
                        else if (format==DataFormat::D32_Float) { bpp=4;dst.format="r32f"; }
                        else continue; // Never copy an unknown format into a guessed-size buffer.
                        if (!device_->GetTextureSize(src.resource,dst.width,dst.height)) continue;
                        dst.bytes=u64(dst.width)*dst.height*bpp;
                    }
                    if (!dst.bytes) continue;
                    BufferDesc bd{};bd.size=dst.bytes;
                    bd.bindFlags=u32(BufferUsageFlags::TransferDst);bd.memoryUsage=GPUMemoryUsage::Readback;
                    dst.staging=device_->CreateBuffer(bd);
                    if (dst.staging==handles::INVALID_RESOURCE) continue;
                    if (!src.bpp) cmd->CopyBuffer(src.resource,dst.staging,0,0,dst.bytes);
                    else {
                        ResourceBarrier bar{};bar.resource=src.resource;
                        bar.beforeState=ResourceState::ShaderResource;bar.afterState=ResourceState::CopySource;
                        cmd->InsertBarrier(&bar,1);
                        BufferTextureCopyRegion rr{};rr.imageSubresource.layerCount=1;
                        rr.imageExtent={dst.width,dst.height,1};
                        cmd->CopyTextureToBuffer(src.resource,dst.staging,&rr,1);
                        bar.beforeState=ResourceState::CopySource;bar.afterState=ResourceState::ShaderResource;
                        cmd->InsertBarrier(&bar,1);
                    }
                }
                cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,
                    AccessFlag::TransferWrite,AccessFlag::HostRead);
                gather_audit_frame_=inputs.frame_index;gather_audit_slot_=bindingIdx;
                gather_audit_queued_=true;
            }

            // SC_DUMP_CARDS=1 — deferred luminance stats of the gather output
            // (Fusion's DDGI input). Localizes a "screen went black" drain to
            // before/after this stage; probes vs output vs screen.
            static const bool dumpOut = [] {
                const char* e = std::getenv("SC_DUMP_CARDS");
                return e && atoi(e) != 0;
            }();
            static u32 tick = 0;
            ++tick;
            if (dumpOut) {
                const u32 dHalfW = gather_width_;
                const u32 dHalfH = gather_height_;
                if (out_dump_frame_ != 0xFFFFFFFFu && tick > out_dump_frame_ + 3u) {
                    if (auto* texels = static_cast<const u16*>(
                            device_->MapBuffer(out_staging_))) {
                        auto f16 = [](u16 h) -> float {
                            u32 sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu,
                                mant = h & 0x3FFu;
                            if (exp == 0u) return 0.0f;
                            u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
                            float f; std::memcpy(&f, &bits, 4);
                            return f;
                        };
                        std::vector<float> lums;
                        lums.reserve((size_t)dHalfW * dHalfH / 8);
                        for (u32 y = 0; y < dHalfH; y += 3)
                            for (u32 x = 0; x < dHalfW; x += 3) {
                                const u16* t = texels + ((size_t)y * dHalfW + x) * 4;
                                float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                                if (std::isnan(r) || std::isnan(g) || std::isnan(b)) continue;
                                lums.push_back(0.2126f * r + 0.7152f * g + 0.0722f * b);
                            }
                        if (!lums.empty()) {
                            std::sort(lums.begin(), lums.end());
                            auto pct = [&](float q) {
                                return lums[std::min(lums.size() - 1,
                                                     (size_t)(q * (lums.size() - 1)))];
                            };
                            PRIMAL_LOG_STREAM(Error, "Render") << "[GIGather] out lum p10=" << pct(0.10f)
                                      << " p50=" << pct(0.50f)
                                      << " p90=" << pct(0.90f)
                                      << " max=" << lums.back() << std::endl;
                        }
                        device_->UnmapBuffer(out_staging_);
                    }
                    out_dump_frame_ = 0xFFFFFFFFu;
                }
                if (tick % 90u == 0u && out_dump_frame_ == 0xFFFFFFFFu) {
                    const u64 size = (u64)dHalfW * dHalfH * 8u;   // RGBA16F
                    if (out_staging_ == rhi::handles::INVALID_RESOURCE ||
                        out_staging_size_ < size) {
                        if (out_staging_ != rhi::handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(out_staging_);
                        rhi::BufferDesc d{};
                        d.size = size;
                        d.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                        d.usage = rhi::GPUMemoryUsage::Readback;
                        d.memoryUsage = rhi::GPUMemoryUsage::Readback;
                        d.name = "GIGather_OutDump";
                        out_staging_ = device_->CreateBuffer(d);
                        out_staging_size_ = size;
                    }
                    if (out_staging_ != rhi::handles::INVALID_RESOURCE) {
                        // TEMP diagnosis: raw dump of the probe irradiance
                        // buffer AS THE GATHER READS IT (same slot, buffer
                        // copy — bypasses every sampling/stride question).
                        {
                            auto irrHandle = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx);
                            if (irrHandle != rhi::handles::INVALID_RESOURCE) {
                                const u64 irrBytes = 16384ull * 9u * 16u;
                                static rhi::ResourceHandle irrStaging = rhi::handles::INVALID_RESOURCE;
                                static u64 irrStagingSize = 0;
                                if (irrStaging == rhi::handles::INVALID_RESOURCE ||
                                    irrStagingSize < irrBytes) {
                                    if (irrStaging != rhi::handles::INVALID_RESOURCE)
                                        device_->DestroyBuffer(irrStaging);
                                    rhi::BufferDesc id{};
                                    id.size = irrBytes;
                                    id.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                                    id.usage = rhi::GPUMemoryUsage::Readback;
                                    id.memoryUsage = rhi::GPUMemoryUsage::Readback;
                                    id.name = "GIGather_IrrDump";
                                    irrStaging = device_->CreateBuffer(id);
                                    irrStagingSize = irrBytes;
                                }
                                if (irrStaging != rhi::handles::INVALID_RESOURCE) {
                                    cmd->CopyBuffer(irrHandle, irrStaging, 0, 0, irrBytes);
                                    static u64 s_irrDumpTickBase = 0;
                                    s_irrDumpTickBase = tick; (void)s_irrDumpTickBase;
                                    // Read next frame's stats print (delayed) —
                                    // reuse out_staging deferred pattern via a
                                    // simple 2-tick delayed map is complex; do
                                    // an immediate Wait+Map on a throwaway CB.
                                    auto chI = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
                                    auto* cI = rhi::GetCommandBuffer(chI);
                                    if (cI) {
                                        cI->Begin();
                                        cI->CopyBuffer(irrHandle, irrStaging, 0, 0, irrBytes);
                                        cI->End(); cI->Submit(); cI->WaitForCompletion();
                                        device_->DestroyCommandBuffer(chI);
                                        if (auto* q = static_cast<const float*>(
                                                device_->MapBuffer(irrStaging, 0, irrBytes))) {
                                            float maxAbs = 0.0f; u64 bad = 0;
                                            // 逐波段 maxAbs（band0=L0, 1-3=L1, 4-8=L2）
                                            // + 全局 argmax 探针——把怪物值定位到具体波段。
                                            float bandMax[9] = {}; double bandSum[9] = {};
                                            float worst[4] = {}; u32 worstProbe = 0;
                                            for (u64 p = 0; p < 16384ull; ++p) {
                                                const float* c = q + p * 36;
                                                float pAbs = 0.0f;
                                                for (u32 k = 0; k < 36; ++k) {
                                                    float v = c[k];
                                                    if (std::isnan(v) || std::isinf(v)) { ++bad; continue; }
                                                    float a = std::abs(v);
                                                    pAbs = std::max(pAbs, a);
                                                    maxAbs = std::max(maxAbs, a);
                                                    u32 band = k / 4;
                                                    bandMax[band] = std::max(bandMax[band], a);
                                                    bandSum[band] += a;
                                                }
                                                if (pAbs > worst[0]) {
                                                    worst[0] = pAbs; worstProbe = (u32)p;
                                                    for (int k = 0; k < 3; ++k) worst[1 + k] = c[k];
                                                }
                                            }
                                            float l0Mean = bandSum[0] / 16384.0f;
                                            float l1Mean = 0.0f, l2Mean = 0.0f;
                                            for (int b = 1; b < 4; ++b) l1Mean += bandSum[b];
                                            for (int b = 4; b < 9; ++b) l2Mean += bandSum[b];
                                            l1Mean /= 3.0f * 16384.0f; l2Mean /= 5.0f * 16384.0f;
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  band means: L0=" << l0Mean
                                                      << " L1=" << l1Mean << " L2=" << l2Mean
                                                      << " |L1|/|L0|=" << (l0Mean > 1e-6f ? l1Mean / l0Mean : 0.0f) << std::endl;
                                            double checksum = 0.0;
                                            for (u64 k = 0; k < irrBytes / 4; ++k) {
                                                float v = q[k];
                                                if (!std::isnan(v) && !std::isinf(v)) checksum += std::abs(v);
                                            }
                                            PRIMAL_LOG_STREAM(Error, "Render") << "[IRR_DUMP] slot=" << ddgiReadIdx
                                                      << " maxAbs=" << maxAbs
                                                      << " nanInf=" << bad
                                                      << " chk=" << (long long)checksum << std::endl;
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  bands maxAbs: L0=" << bandMax[0]
                                                      << " L1=(" << bandMax[1] << "," << bandMax[2] << "," << bandMax[3] << ")"
                                                      << " L2=(" << bandMax[4] << "," << bandMax[5] << "," << bandMax[6]
                                                      << "," << bandMax[7] << "," << bandMax[8] << ")" << std::endl;
                                            // 深度图健康检查：可见性加权的输入。
                                            // 全零/全 1 → UpdateDepth 未生效。
                                            if (auto* dp = static_cast<const float*>(
                                                    device_->MapBuffer(ddgi->GetDepthBufferBySlot(ddgiReadIdx), 0, 16384ull * 128u * 4u))) {
                                                u64 zeroMean = 0; float meanSum = 0.0f, varSum = 0.0f;
                                                for (u64 pr = 0; pr < 16384ull; ++pr) {
                                                    const float* dm = dp + pr * 128u;
                                                    float m = dm[0]; // 取 64 mean 中首个代表
                                                    for (int k = 1; k < 64; ++k) m += dm[k];
                                                    m /= 64.0f;
                                                    if (m <= 1e-6f) ++zeroMean;
                                                    meanSum += m;
                                                    for (int k = 64; k < 128; ++k) varSum += dm[k];
                                                }
                                                PRIMAL_LOG_STREAM(Error, "Render") << "  [DEPTH_DUMP] zeroMeanProbes=" << zeroMean
                                                          << "/16384 meanOfMeans=" << meanSum / 16384.0f
                                                          << " avgVar=" << varSum / (16384.0f * 64.0f) << std::endl;
                                                device_->UnmapBuffer(ddgi->GetDepthBufferBySlot(ddgiReadIdx));
                                            }
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  worstProbe=" << worstProbe
                                                      << " abs=" << worst[0]
                                                      << " L0rgb=(" << worst[1] << "," << worst[2] << "," << worst[3] << ")" << std::endl;
                                            device_->UnmapBuffer(irrStaging);
                                        }
                                    }
                                }
                            }
                        }
                        ResourceBarrier toCopy{};
                        toCopy.resource = gi_halfres_texture_;
                        toCopy.beforeState = ResourceState::ShaderResource;
                        toCopy.afterState = ResourceState::CopySource;
                        toCopy.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toCopy, 1);

                        rhi::BufferTextureCopyRegion region{};
                        region.bufferOffset = 0;
                        region.bufferRowLength = dHalfW;
                        region.bufferImageHeight = dHalfH;
                        region.imageSubresource.mipLevel = 0;
                        region.imageSubresource.baseArrayLayer = 0;
                        region.imageSubresource.layerCount = 1;
                        region.imageOffset = {0, 0, 0};
                        region.imageExtent = {dHalfW, dHalfH, 1};
                        cmd->CopyTextureToBuffer(gi_halfres_texture_, out_staging_,
                                                 &region, 1);

                        ResourceBarrier toSrv{};
                        toSrv.resource = gi_halfres_texture_;
                        toSrv.beforeState = ResourceState::CopySource;
                        toSrv.afterState = ResourceState::ShaderResource;
                        toSrv.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toSrv, 1);
                        out_dump_frame_ = tick;
                    }
                }
            }
        }
    );

    return outputs;
}

} // namespace primal::graphics
