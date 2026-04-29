#include "LumenDDGIPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include <algorithm>

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

namespace {

// ============================================================================
// Descriptor update helper (from TestNaniteStreamingPipeline pattern)
// ============================================================================

struct DescriptorData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

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

static const std::string SHADER_BASE_DIR = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
static const std::string DDGI_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Recursively resolve #include "..." directives by inlining file content.
// Only resolves local includes (quoted), not system includes (angle brackets).
static std::string ResolveIncludes(const std::string& source, const std::string& baseDir,
                                   std::set<std::string>& included) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;

    while (std::getline(in, line)) {
        // Check for #include "..." (local include, not <...> system include)
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) trimmed = trimmed.substr(firstNonSpace);

        if (trimmed.find("#include \"") == 0) {
            size_t start = trimmed.find('"') + 1;
            size_t end = trimmed.find('"', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string includeFile = trimmed.substr(start, end - start);

                // Search in baseDir first, then SHADER_BASE_DIR
                std::string fullPath = baseDir + includeFile;
                if (std::ifstream(fullPath).good() == false) {
                    fullPath = SHADER_BASE_DIR + includeFile;
                }

                if (included.find(fullPath) == included.end()) {
                    included.insert(fullPath);
                    std::string includedContent = ReadFileToString(fullPath);
                    if (!includedContent.empty()) {
                        // Recursively resolve includes in the included file
                        std::string resolved = ResolveIncludes(includedContent,
                            fullPath.substr(0, fullPath.find_last_of('/') + 1), included);
                        out << resolved << "\n";
                    } else {
                        std::cerr << "[LumenDDGI] Warning: Failed to read include: " << fullPath << std::endl;
                        out << line << "\n";
                    }
                }
                // If already included, skip (header guard / #pragma once handles it)
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

static std::vector<u8> LoadShaderBytecode(const char* shaderName) {
    std::string shaderPath = DDGI_SHADER_DIR + shaderName + ".metal";

    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        std::cerr << "[LumenDDGI] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    // Resolve all #include "..." directives by inlining
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, DDGI_SHADER_DIR, included);

    // DEBUG: Dump resolved source to file
    {
        std::string dumpPath = "/tmp/ddgi_" + std::string(shaderName) + ".resolved.metal";
        std::ofstream dumpFile(dumpPath);
        if (dumpFile.is_open()) {
            dumpFile << resolved;
            dumpFile.close();
            std::cerr << "[LumenDDGI] Dumped resolved source to " << dumpPath << " (" << resolved.size() << " bytes)" << std::endl;
        }
    }

    return std::vector<u8>(resolved.begin(), resolved.end());
}

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

LumenDDGIPass::~LumenDDGIPass() {
    Shutdown();
}

bool LumenDDGIPass::Initialize(RHIDeviceBase* device, const DDGIRuntimeParams& params) {
    if (initialized_) return true;

    device_ = device;
    params_ = params;

    // Pre-compute probe origin at the center of the grid
    probe_origin_ = {
        -(float)(params_.probe_count_x - 1) * params_.probe_spacing * 0.5f,
        -(float)(params_.probe_count_y - 1) * params_.probe_spacing * 0.5f,
        -(float)(params_.probe_count_z - 1) * params_.probe_spacing * 0.5f,
    };

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create compute pipelines + descriptor sets
    CreatePipelines();

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create persistent probe textures
    CreateProbeTextures();

    initialized_ = true;

    u32 totalProbes = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    std::cout << "[LumenDDGI] Initialized (grid: "
              << params_.probe_count_x << "x" << params_.probe_count_y << "x" << params_.probe_count_z
              << " = " << totalProbes << " probes, "
              << params_.rays_per_probe << " rays/probe)" << std::endl;
    return true;
}

void LumenDDGIPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    device_ = nullptr;

    // No explicit GPU resource destruction needed -- handles are POD types
    // managed by the RHI device's garbage collector.
}

// ============================================================================
// Private helper methods
// ============================================================================

void LumenDDGIPass::CreateDescriptorSetLayouts() {
    // --- Trace: 3 SDF textures (sampled) + 1 prev_color (sampled) + 2 UBO + 1 SSBO ---
    // Metal uses SEPARATE binding namespaces for textures and buffers.
    // [[texture(N)]] and [[buffer(N)]] are independent.
    // So binding 0 can be used for BOTH texture(0) and buffer(0).
    {
        DescriptorSetLayoutBinding traceBindings[] = {
            // Textures (sampled) — SDF cascades + prev frame color
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev frame color (lit scene)
            // Buffers (separate Metal namespace)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data
        };
        DescriptorSetLayoutDesc layoutDesc{7, traceBindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Irradiance: 2 UBO + 3 SSBO (ray data + irradiance history + irradiance output) ---
    {
        DescriptorSetLayoutBinding irradianceBindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance history buffer
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance output buffer
        };
        DescriptorSetLayoutDesc layoutDesc{5, irradianceBindings};
        irradiance_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Depth: 2 UBO + 3 SSBO (ray data + depth history + depth output) ---
    // Buffer-based: no texture3D, uses storage buffer for Apple Silicon performance
    {
        DescriptorSetLayoutBinding depthBindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // depth history buffer
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // depth output buffer
        };
        DescriptorSetLayoutDesc layoutDesc{5, depthBindings};
        depth_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void LumenDDGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    // Compile shaders
    auto traceShader = CompileShader("DDGITraceRays", "ddgi_trace_rays");
    auto irradianceShader = CompileShader("DDGIUpdateIrradiance", "ddgi_update_irradiance");
    auto depthShader = CompileShader("DDGIUpdateDepth", "ddgi_update_depth");

    if (traceShader == handles::INVALID_SHADER ||
        irradianceShader == handles::INVALID_SHADER ||
        depthShader == handles::INVALID_SHADER) {
        std::cerr << "[LumenDDGI] Shader compilation failed" << std::endl;
        return;
    }

    // Create pipeline layouts
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

    // Create compute pipelines
    // Trace uses threadGroupSize (64,1,1) for ray-level parallelism
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // Irradiance update uses (64,1,1) — one thread per probe
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = irradianceShader;
        pipeDesc.layout = irradiance_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        irradiance_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // Depth update uses (64,1,1) — one thread per probe
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = depthShader;
        pipeDesc.layout = depth_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
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
            cbs[i] = device_->CreateBuffer(desc);
        }
    };

    CreateCBs(global_cb_, 512);          // GlobalShaderData (432 bytes, padded)
    CreateCBs(volume_cb_, 512);          // DDGIVolumeData
}

void LumenDDGIPass::CreateProbeTextures() {
    u32 nx = params_.probe_count_x;
    u32 ny = params_.probe_count_y;
    u32 nz = params_.probe_count_z;

    // Irradiance storage buffers: each probe stores 9 SH3 coefficients (float3 each)
    u32 totalProbes = nx * ny * nz;
    u64 irradianceSize = (u64)totalProbes * 9 * sizeof(float) * 3;

    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = irradianceSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 9 * 3;
        desc.structured.elementStride = sizeof(float);
        irradiance_buffers_[i] = device_->CreateBuffer(desc);

        // Zero-initialize to prevent garbage data causing flickering
        void* mapped = device_->MapBuffer(irradiance_buffers_[i]);
        if (mapped) {
            memset(mapped, 0, irradianceSize);
            device_->UnmapBuffer(irradiance_buffers_[i]);
        }
    }

    // Depth buffers: float[probeCount * 8], one float per octant per probe
    // Apple Silicon: buffer load/store is much faster than texture3D sampling
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = totalProbes * 8 * sizeof(float);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 8;
        desc.structured.elementStride = sizeof(float);
        depth_buffers_[i] = device_->CreateBuffer(desc);

        // Zero-initialize
        void* mapped = device_->MapBuffer(depth_buffers_[i]);
        if (mapped) {
            memset(mapped, 0, totalProbes * 8 * sizeof(float));
            device_->UnmapBuffer(depth_buffers_[i]);
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
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(DDGIRayData);
        ray_data_buffer_ = device_->CreateBuffer(desc);
    }
}

// ============================================================================
// AddPass -- main entry point called per frame
// ============================================================================

bool LumenDDGIPass::UpdateProbeOrigin(const math::v3& camera_position)
{
    if (!initialized_) return false;

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
    u32 current_frame_index)
{
    LumenDDGIOutput output{};

    u32 outIdx = current_frame_index % 3;
    u32 histIdx = (current_frame_index + 2) % 3;

    // Import persistent probe buffers/textures into render graph
    auto irradianceOutHandle = graph.ImportResource(
        "LumenDDGI_Irradiance_" + std::to_string(outIdx), irradiance_buffers_[outIdx]);
    auto irradianceHistHandle = graph.ImportResource(
        "LumenDDGI_IrradianceHist_" + std::to_string(histIdx), irradiance_buffers_[histIdx]);
    auto depthOutHandle = graph.ImportResource(
        "LumenDDGI_Depth_" + std::to_string(outIdx), depth_buffers_[outIdx]);
    auto depthHistHandle = graph.ImportResource(
        "LumenDDGI_DepthHist_" + std::to_string(histIdx), depth_buffers_[histIdx]);

    output.ddgi_irradiance = irradianceOutHandle;
    output.ddgi_irradiance_hist = irradianceHistHandle;

    graph.AddPass<LumenDDGIData>("LumenDDGI",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, irradianceOutHandle, irradianceHistHandle,
         depthOutHandle, depthHistHandle](
            LumenDDGIData& data, RenderGraphBuilder& builder) {
            // Read previous frame color (for lighting lookup during trace)
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;

            // Write to output irradiance/depth textures
            builder.Write(irradianceOutHandle, ResourceState::UnorderedAccess);
            builder.Read(irradianceHistHandle, ResourceState::ShaderResource);
            builder.Write(depthOutHandle, ResourceState::UnorderedAccess);
            builder.Read(depthHistHandle, ResourceState::ShaderResource);
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

            u32 frameIdx = current_frame_index % 3;

            u32 probeCountTotal = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;

            // Resolve physical handles from render graph
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle prevColorTex = ResolveTexture(prev_frame_color);

            // Resolve SDF cascade textures from GlobalSDF
            auto& sdf = nanite::GlobalSDF::Get();
            bool sdfAvailable = sdf.IsInitialized();
            ResourceHandle sdfTextures[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
            };
            if (sdfAvailable) {
                for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                    sdfTextures[c] = sdf.GetCascade(c).sdf_texture;
                }
            }

            // DEBUG: Log DDGI state (first 10 frames)
            static u32 dbgFrame = 0;
            if (dbgFrame < 10) {
                // Log SDF cascade data being passed to DDGI
                auto& sdfRef = nanite::GlobalSDF::Get();
                std::cout << "[LumenDDGI] Frame " << dbgFrame
                          << " frameIdx=" << frameIdx
                          << " probeCount=" << probeCountTotal
                          << " rays/probe=" << params_.rays_per_probe
                          << "\n  sdfAvailable=" << (sdfAvailable ? "YES" : "NO (skipping trace)")
                          << " sdfCascadeCount=" << (sdfAvailable ? sdfRef.GetConfig().cascade_count : 0)
                          << "\n  prevColorTex=" << prevColorTex
                          << " ray_data_buffer=" << ray_data_buffer_
                          << "\n  irradiance_out=" << irradiance_buffers_[outIdx]
                          << " irradiance_hist=" << irradiance_buffers_[histIdx]
                          << "\n  depth_out=" << depth_buffers_[outIdx]
                          << " depth_hist=" << depth_buffers_[histIdx]
                          << "\n  sdf[0]=" << sdfTextures[0]
                          << " sdf[1]=" << sdfTextures[1]
                          << " sdf[2]=" << sdfTextures[2];
                if (sdfAvailable && sdfRef.GetConfig().cascade_count > 0) {
                    const auto& c0 = sdfRef.GetCascade(0);
                    std::cout << "\n  cascade0_origin=(" << c0.origin.x << "," << c0.origin.y << "," << c0.origin.z << ")"
                              << " extent=(" << c0.extent.x << "," << c0.extent.y << "," << c0.extent.z << ")"
                              << " voxelSize=" << c0.voxel_size
                              << " res=" << c0.resolution
                              << " mipLevels=" << c0.mip_levels;
                }
                std::cout << std::endl;
                dbgFrame++;
            }

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
                if (mapped) {
                    mapped->View = camera_data.view_matrix;
                    mapped->Projection = camera_data.proj_matrix;
                    mapped->InvProjection = rhi::math::Inverse(camera_data.proj_matrix);
                    mapped->ViewProjection = camera_data.proj_matrix * camera_data.view_matrix;
                    mapped->PreviousViewProjection = camera_data.prev_proj_matrix * camera_data.prev_view_matrix;
                    mapped->InvViewProjection = rhi::math::Inverse(mapped->ViewProjection);

                    math::m4x4 invView = rhi::math::Inverse(camera_data.view_matrix);
                    mapped->CameraPositionAndViewWidth = {
                        invView.columns[3][0], invView.columns[3][1],
                        invView.columns[3][2], (float)0.0f  // No screen dimensions needed for DDGI
                    };

                    auto& vm = camera_data.view_matrix;
                    mapped->CameraDirectionAndViewHeight = {
                        -vm.columns[2][0], -vm.columns[2][1],
                        -vm.columns[2][2], 0.0f
                    };

                    mapped->NumDirectionalLights = 0;
                    mapped->DeltaTime = camera_data.delta_time;
                    mapped->FrameCount = (float)camera_data.frame_index;
                    device_->UnmapBuffer(global_cb_[frameIdx]);
                }
            }

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
                    // ProbeCounts[3] auto-zeroed (padding)
                    vd.RaysPerProbe = params_.rays_per_probe;
                    vd.ProbeCountTotal = probeCountTotal;
                    vd.IrradianceBlurSigma = params_.irradiance_temporal_weight;
                    vd.DepthBlurSigma = params_.depth_temporal_weight;
                    vd.DeltaTime = camera_data.delta_time;
                    vd.FrameIndex = camera_data.frame_index;
                    vd.RayMaxDistance = params_.ray_max_distance;
                    vd.ProbeHysteresis = 0.08f;
                    vd.TemporalAlpha = 0.1f;
                    vd.LightDirection = {camera_data.light_direction.x,
                                         camera_data.light_direction.y,
                                         camera_data.light_direction.z, 0.0f};
                    vd.LightColor = {camera_data.light_color.x,
                                     camera_data.light_color.y,
                                     camera_data.light_color.z, 0.0f};

                    // Fill SDF cascade data from GlobalSDF
                    for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                        const auto& cascade = sdf.GetCascade(c);
                        vd.SdfOrigins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                        vd.SdfVoxelSizes[c] = {cascade.voxel_size, 0.0f, 0.0f, 0.0f};
                        vd.SdfExtents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                        vd.SdfResolutions[c] = cascade.resolution;
                    }
                    vd.SdfCascadeCount = sdf.GetConfig().cascade_count;

                    *mapped = vd;
                    device_->UnmapBuffer(volume_cb_[frameIdx]);

                    // Store for C++ access via GetVolumeData()
                    volume_data_ = vd;
                }
            }

            // ================================================================
            // Sub-pass 1: TraceRays (skip if GlobalSDF not available)
            // ================================================================
            if (sdfAvailable &&
                trace_pipeline_ != handles::INVALID_PIPELINE &&
                ray_data_buffer_ != handles::INVALID_RESOURCE) {
                // Update trace descriptor set
                DescriptorData traceParams[] = {
                    // Textures: SDF cascades + prev frame lit scene color
                    {0, DescriptorType::SampledImage,  sdfTextures[0]},
                    {1, DescriptorType::SampledImage,  sdfTextures[1]},
                    {2, DescriptorType::SampledImage,  sdfTextures[2]},
                    {3, DescriptorType::SampledImage,  prevColorTex},
                    // Metal: buffers use separate binding namespace from textures
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, ray_data_buffer_},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 7);

                cmd->BindComputePipeline(trace_pipeline_);
                const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);

                u32 totalRayThreads = probeCountTotal * params_.rays_per_probe;
                u32 gx = (totalRayThreads + 63) / 64;  // threadGroupSize = (64,1,1)

                // DEBUG: Log first 3 frames
                static u32 traceFrame = 0;
                if (traceFrame < 3) {
                    std::cout << "[LumenDDGI] TRACE dispatch: gx=" << gx
                              << " totalRayThreads=" << totalRayThreads
                              << " probeCountTotal=" << probeCountTotal
                              << " raysPerProbe=" << params_.rays_per_probe
                              << std::endl;
                    traceFrame++;
                }

                cmd->Dispatch(gx, 1, 1);
            }

            // Barrier: ray data Storage -> ShaderResource (for irradiance/depth update reads)
            {
                ResourceBarrier barrier{};
                barrier.resource = ray_data_buffer_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 2: UpdateIrradiance
            // ================================================================
            if (irradiance_pipeline_ != handles::INVALID_PIPELINE &&
                irradiance_buffers_[outIdx] != handles::INVALID_RESOURCE) {
                // Update irradiance descriptor set
                DescriptorData irradianceParams[] = {
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, ray_data_buffer_},
                    {3, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                    {4, DescriptorType::StorageBuffer, irradiance_buffers_[outIdx]},
                };
                UpdateDescriptorSet(device_, irradiance_ds_[frameIdx], irradianceParams, 5);

                cmd->BindComputePipeline(irradiance_pipeline_);
                const DescriptorSetHandle sets[] = { irradiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, irradiance_layout_, 0, 1, sets, 0, nullptr);

                cmd->Dispatch((probeCountTotal + 63) / 64, 1, 1);
            }

            // Barrier: irradiance UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = irradiance_buffers_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 3: UpdateDepth (buffer-based, no texture3D)
            // ================================================================
            if (depth_pipeline_ != handles::INVALID_PIPELINE &&
                depth_buffers_[outIdx] != handles::INVALID_RESOURCE) {
                // Update depth descriptor set (buffers only, no textures)
                DescriptorData depthParams[] = {
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, ray_data_buffer_},
                    {3, DescriptorType::StorageBuffer, depth_buffers_[histIdx]},  // history
                    {4, DescriptorType::StorageBuffer, depth_buffers_[outIdx]},   // output
                };
                UpdateDescriptorSet(device_, depth_ds_[frameIdx], depthParams, 5);

                cmd->BindComputePipeline(depth_pipeline_);
                const DescriptorSetHandle sets[] = { depth_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, depth_layout_, 0, 1, sets, 0, nullptr);

                cmd->Dispatch((probeCountTotal + 63) / 64, 1, 1);
            }

            // Barrier: depth buffer UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = depth_buffers_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
