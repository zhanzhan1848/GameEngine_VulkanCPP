#include "ScreenProbeGIPass.h"
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
// Descriptor update helper
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
// ============================================================================

static const std::string SHADER_BASE_DIR = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
static const std::string SCREEN_PROBE_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::string ResolveIncludes(const std::string& source, const std::string& baseDir,
                                   std::set<std::string>& included) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) trimmed = trimmed.substr(firstNonSpace);

        if (trimmed.find("#include \"") == 0) {
            size_t start = trimmed.find('"') + 1;
            size_t end = trimmed.find('"', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string includeFile = trimmed.substr(start, end - start);
                std::string fullPath = baseDir + includeFile;
                if (std::ifstream(fullPath).good() == false) {
                    fullPath = SHADER_BASE_DIR + includeFile;
                }
                if (included.find(fullPath) == included.end()) {
                    included.insert(fullPath);
                    std::string includedContent = ReadFileToString(fullPath);
                    if (!includedContent.empty()) {
                        std::string resolved = ResolveIncludes(includedContent,
                            fullPath.substr(0, fullPath.find_last_of('/') + 1), included);
                        out << resolved << "\n";
                    }
                }
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

static std::vector<u8> LoadShaderBytecode(const char* shaderName) {
    std::string shaderPath = SCREEN_PROBE_SHADER_DIR + shaderName + ".metal";
    std::string source = ReadFileToString(shaderPath);

    if (source.empty()) {
        std::cerr << "[ScreenProbeGI] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, SCREEN_PROBE_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct ScreenProbePassData {
    RGResourceHandle gbuffer_depth;
    RGResourceHandle gbuffer_normal;
    RGResourceHandle prev_frame_color;
    RGResourceHandle gi_output;
};

} // anonymous namespace

// ============================================================================
// ScreenProbeGIPass Implementation
// ============================================================================

ScreenProbeGIPass::~ScreenProbeGIPass() {
    Shutdown();
}

bool ScreenProbeGIPass::Initialize(RHIDeviceBase* device,
                                    u32 render_width, u32 render_height,
                                    const ScreenProbeParams& params) {
    if (initialized_) return true;

    device_ = device;
    params_ = params;
    render_width_ = render_width;
    render_height_ = render_height;

    // Compute probe grid dimensions
    grid_width_ = (render_width + params_.downsample_factor - 1) / params_.downsample_factor;
    grid_height_ = (render_height + params_.downsample_factor - 1) / params_.downsample_factor;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreateBuffers();

    initialized_ = true;

    u32 totalProbes = grid_width_ * grid_height_;
    std::cout << "[ScreenProbeGI] Initialized (grid: "
              << grid_width_ << "x" << grid_height_
              << " = " << totalProbes << " probes, "
              << params_.rays_per_probe << " rays/probe, "
              << "downsample=" << params_.downsample_factor << ")" << std::endl;
    return true;
}

void ScreenProbeGIPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

// ============================================================================
// Private helper methods
// ============================================================================

void ScreenProbeGIPass::CreateDescriptorSetLayouts() {
    // --- Place: 2 textures (depth, normal) + 2 SSBO (positions, normals) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        place_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Trace: 4 textures (3 SDF + prev_color) + 3 SSBO (positions, normals, radiance) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures (SDF cascades + prev frame color)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 2
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev frame color
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (write)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Gather: 2 textures (depth, normal) + 1 storage image (output) + 3 SSBO + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output texture
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (read)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
        };
        DescriptorSetLayoutDesc layoutDesc{7, bindings};
        gather_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Average: 2 SSBO (rayRadiance in, probeAvg out) + 2 uniform (totalProbes, raysPerProbe) ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // avg radiance (write)
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // totalProbes + raysPerProbe
        };
        DescriptorSetLayoutDesc layoutDesc{3, bindings};
        avg_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Temporal: 5 SSBO + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current avg radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history avg radiance (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current positions (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history positions (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // output radiance (write)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // temporal constants
        };
        DescriptorSetLayoutDesc layoutDesc{6, bindings};
        temporal_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void ScreenProbeGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto placeShader = CompileShader("ScreenProbePlace", "screen_probe_place");
    auto traceShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_rays");
    auto avgShader = CompileShader("ScreenProbeAverage", "screen_probe_average");
    if (avgShader == handles::INVALID_SHADER) {
        std::cerr << "[ScreenProbeGI] Average shader compilation failed" << std::endl;
        return;
    }
    auto gatherShader = CompileShader("ScreenProbeGather", "screen_probe_gather");
    auto temporalShader = CompileShader("ScreenProbeTemporal", "screen_probe_temporal");

    if (placeShader == handles::INVALID_SHADER ||
        traceShader == handles::INVALID_SHADER ||
        gatherShader == handles::INVALID_SHADER ||
        temporalShader == handles::INVALID_SHADER) {
        std::cerr << "[ScreenProbeGI] Shader compilation failed" << std::endl;
        return;
    }

    // Create pipeline layouts
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &place_set_layout_;
        place_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &trace_set_layout_;
        trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &avg_set_layout_;
        avg_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &gather_set_layout_;
        gather_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &temporal_set_layout_;
        temporal_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = placeShader;
        pipeDesc.layout = place_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        place_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = avgShader;
        pipeDesc.layout = avg_layout_;
        // ThreadGroup size must match raysPerProbe for cooperative reduction
        pipeDesc.threadGroupSize = {params_.rays_per_probe, 1, 1};
        avg_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = gatherShader;
        pipeDesc.layout = gather_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        gather_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = temporalShader;
        pipeDesc.layout = temporal_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        temporal_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{place_set_layout_};
            place_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{trace_set_layout_};
            trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{avg_set_layout_};
            avg_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{gather_set_layout_};
            gather_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{temporal_set_layout_};
            temporal_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void ScreenProbeGIPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 512;  // ScreenProbeGlobalData padded
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        global_cb_[i] = device_->CreateBuffer(desc);
    }
}

void ScreenProbeGIPass::CreateBuffers() {
    u32 totalProbes = grid_width_ * grid_height_;

    // Probe positions buffers (triple-buffered): float4 per probe
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_positions_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_positions_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_positions_buffer_[i]);
        }
    }

    // Probe normals buffers (triple-buffered): float4 per probe
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_normals_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_normals_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_normals_buffer_[i]);
        }
    }

    // Probe radiance buffers (triple-buffered): float4 per ray
    for (int i = 0; i < 3; ++i) {
        u64 radianceSize = (u64)totalProbes * params_.rays_per_probe * sizeof(float) * 4;
        BufferDesc desc{};
        desc.size = radianceSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_radiance_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_radiance_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, radianceSize);
            device_->UnmapBuffer(probe_radiance_buffer_[i]);
        }
    }

    // Per-probe average radiance buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_avg_radiance_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_avg_radiance_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_avg_radiance_[i]);
        }
    }

    // Average pass constant buffers (triple-buffered, 2 x uint32)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 16;  // 2 x uint32, padded to 16 bytes
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        avg_constants_[i] = device_->CreateBuffer(desc);

        struct AvgCB { u32 totalProbes; u32 raysPerProbe; };
        auto* mapped = static_cast<AvgCB*>(device_->MapBuffer(avg_constants_[i]));
        if (mapped) {
            mapped->totalProbes = totalProbes;
            mapped->raysPerProbe = params_.rays_per_probe;
            device_->UnmapBuffer(avg_constants_[i]);
        }
    }

    // Temporal accumulation constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 16;  // totalProbes(u32) + alpha(f32) + threshold(f32) + pad(u32)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        temporal_cb_[i] = device_->CreateBuffer(desc);
    }

    // Output texture: full-resolution RGBA16_Float
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        output_texture_ = device_->CreateTexture(desc);
    }
}

// ============================================================================
// AddPass -- main entry point called per frame
// ============================================================================

ScreenProbeGIOutput ScreenProbeGIPass::AddPass(
    RenderGraph& graph,
    RGResourceHandle gbuffer_depth,
    RGResourceHandle gbuffer_normal,
    RGResourceHandle prev_frame_color,
    const ScreenProbeCameraData& camera_data,
    u32 current_frame_index)
{
    ScreenProbeGIOutput output{};

    u32 frameIdx = current_frame_index % 3;
    u32 histIdx = (current_frame_index + 2) % 3;

    // Import persistent output texture into render graph
    auto giOutputHandle = graph.ImportResource("ScreenProbeGI_Output", output_texture_);
    output.gi_output = giOutputHandle;

    graph.AddPass<ScreenProbePassData>("ScreenProbeGI",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [gbuffer_depth, gbuffer_normal, prev_frame_color, giOutputHandle](
            ScreenProbePassData& data, RenderGraphBuilder& builder) {
            // Read GBuffer inputs
            builder.Read(gbuffer_depth, ResourceState::ShaderResource);
            builder.Read(gbuffer_normal, ResourceState::ShaderResource);

            // Read previous frame color for radiance sampling
            if (prev_frame_color.IsValid()) {
                builder.Read(prev_frame_color, ResourceState::ShaderResource);
            }

            // Write to output texture
            builder.Write(giOutputHandle, ResourceState::UnorderedAccess);

            data.gbuffer_depth = gbuffer_depth;
            data.gbuffer_normal = gbuffer_normal;
            data.prev_frame_color = prev_frame_color;
            data.gi_output = giOutputHandle;
        },

        // ====================================================================
        // Execute lambda: dispatch 3 compute sub-passes
        // ====================================================================
        [this, camera_data, current_frame_index, frameIdx, histIdx](
            const ScreenProbePassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            // Resolve physical handles from render graph
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle depthTex = ResolveTexture(data.gbuffer_depth);
            ResourceHandle normalTex = ResolveTexture(data.gbuffer_normal);
            ResourceHandle prevColorTex = ResolveTexture(data.prev_frame_color);
            ResourceHandle outputTex = ResolveTexture(data.gi_output);

            if (depthTex == handles::INVALID_RESOURCE ||
                normalTex == handles::INVALID_RESOURCE ||
                outputTex == handles::INVALID_RESOURCE) {
                return;
            }

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

            // DEBUG: log first 5 frames
            static u32 dbgSpFrame = 0;
            if (dbgSpFrame < 5) {
                std::cout << "[ScreenProbeGI] Frame " << dbgSpFrame
                          << " sdf=" << (sdfAvailable ? "YES" : "NO")
                          << " prevColor=" << prevColorTex
                          << " depth=" << depthTex
                          << " normal=" << normalTex
                          << " output=" << outputTex
                          << " place_pipe=" << place_pipeline_
                          << " trace_pipe=" << trace_pipeline_
                          << " gather_pipe=" << gather_pipeline_
                          << " grid=" << grid_width_ << "x" << grid_height_
                          << std::endl;
                dbgSpFrame++;
            }

            // ---- Upload constant buffer ----
            {
                auto* mapped = static_cast<ScreenProbeGlobalData*>(device_->MapBuffer(global_cb_[frameIdx]));
                if (mapped) {
                    ScreenProbeGlobalData gd{};
                    math::m4x4 vp = camera_data.proj_matrix * camera_data.view_matrix;
                    gd.view_projection = vp;
                    gd.inv_view_projection = rhi::math::Inverse(vp);
                    gd.camera_position = {camera_data.camera_position.x,
                                          camera_data.camera_position.y,
                                          camera_data.camera_position.z, 0.0f};
                    gd.grid_params = {(float)grid_width_, (float)grid_height_,
                                      (float)params_.downsample_factor, (float)params_.rays_per_probe};
                    gd.trace_params = {params_.max_ray_distance, params_.gather_radius,
                                       (float)render_width_, (float)render_height_};

                    // Populate SDF cascade data
                    if (sdfAvailable) {
                        u32 cascadeCount = std::min(3u, sdf.GetConfig().cascade_count);
                        for (u32 c = 0; c < cascadeCount; ++c) {
                            const auto& cascade = sdf.GetCascade(c);
                            gd.sdf_origins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                            gd.sdf_voxel_sizes[c] = {cascade.voxel_size, cascade.voxel_size, cascade.voxel_size, 0.0f};
                            gd.sdf_extents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                        }
                        gd.sdf_resolutions = {
                            cascadeCount > 0 ? (float)sdf.GetCascade(0).resolution : 0.0f,
                            cascadeCount > 1 ? (float)sdf.GetCascade(1).resolution : 0.0f,
                            cascadeCount > 2 ? (float)sdf.GetCascade(2).resolution : 0.0f,
                            (float)cascadeCount
                        };
                    }

                    *mapped = gd;
                    device_->UnmapBuffer(global_cb_[frameIdx]);
                }
            }

            // ==================================================================
            // Sub-pass 1: ScreenProbePlace
            // ==================================================================
            {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  depthTex},
                    {1, DescriptorType::SampledImage,  normalTex},
                    {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, place_ds_[frameIdx], params, 5);

                cmd->BindComputePipeline(place_pipeline_);
                const DescriptorSetHandle sets[] = { place_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, place_layout_, 0, 1, sets, 0, nullptr);

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
            }

            // Barrier: probe buffers written, need to be readable by trace
            {
                ResourceBarrier barriers[2]{};
                barriers[0].resource = probe_positions_buffer_[frameIdx];
                barriers[0].beforeState = ResourceState::UnorderedAccess;
                barriers[0].afterState = ResourceState::ShaderResource;
                barriers[0].subresource = 0xFFFFFFFF;
                barriers[1].resource = probe_normals_buffer_[frameIdx];
                barriers[1].beforeState = ResourceState::UnorderedAccess;
                barriers[1].afterState = ResourceState::ShaderResource;
                barriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(barriers, 2);
            }

            // ==================================================================
            // Sub-pass 2: ScreenProbeTraceRays
            // ==================================================================
            if (sdfAvailable && prevColorTex != handles::INVALID_RESOURCE) {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  sdfTextures[0]},
                    {1, DescriptorType::SampledImage,  sdfTextures[1]},
                    {2, DescriptorType::SampledImage,  sdfTextures[2]},
                    {3, DescriptorType::SampledImage,  prevColorTex},
                    {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], params, 8);

                cmd->BindComputePipeline(trace_pipeline_);
                const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);

                u32 totalRays = grid_width_ * grid_height_ * params_.rays_per_probe;
                u32 dispatchX = (totalRays + 63) / 64;
                cmd->Dispatch(dispatchX, 1, 1);
            }

            // Barrier: radiance buffer written, need to be readable by gather
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_radiance_buffer_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.5: ScreenProbeAverage (pre-aggregate per-ray to per-probe)
            // ==================================================================
            {
                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {2, DescriptorType::UniformBuffer, avg_constants_[frameIdx]},
                };
                UpdateDescriptorSet(device_, avg_ds_[frameIdx], params, 3);

                cmd->BindComputePipeline(avg_pipeline_);
                const DescriptorSetHandle sets[] = { avg_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, avg_layout_, 0, 1, sets, 0, nullptr);

                // One threadgroup per probe (each threadgroup has raysPerProbe threads)
                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch(totalProbes, 1, 1);
            }

            // Barrier: avg radiance buffer written, need to be readable by temporal
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_avg_radiance_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.75: Probe Temporal Accumulation
            // ==================================================================
            if (temporal_pipeline_ != handles::INVALID_PIPELINE) {
                // Upload temporal constants
                struct TemporalCB { u32 totalProbes; float alpha; float threshold; u32 pad; };
                auto* tcb = static_cast<TemporalCB*>(device_->MapBuffer(temporal_cb_[frameIdx]));
                if (tcb) {
                    tcb->totalProbes = grid_width_ * grid_height_;
                    tcb->alpha = 0.1f;
                    tcb->threshold = 0.5f;
                    tcb->pad = 0;
                    device_->UnmapBuffer(temporal_cb_[frameIdx]);
                }

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},   // current (read)
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},    // history (read)
                    {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_positions_buffer_[histIdx]},
                    {4, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},   // output (write, same as current)
                    {5, DescriptorType::UniformBuffer, temporal_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, temporal_ds_[frameIdx], params, 6);

                cmd->BindComputePipeline(temporal_pipeline_);
                const DescriptorSetHandle sets[] = { temporal_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, temporal_layout_, 0, 1, sets, 0, nullptr);

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
            }

            // Barrier: temporal output written, need to be readable by gather
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_avg_radiance_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 3: ScreenProbeGather
            // ==================================================================
            {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  depthTex},
                    {1, DescriptorType::SampledImage,  normalTex},
                    {2, DescriptorType::StorageImage,  outputTex},
                    {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, gather_ds_[frameIdx], params, 7);

                cmd->BindComputePipeline(gather_pipeline_);
                const DescriptorSetHandle sets[] = { gather_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, gather_layout_, 0, 1, sets, 0, nullptr);

                u32 totalPixels = render_width_ * render_height_;
                cmd->Dispatch((totalPixels + 63) / 64, 1, 1);
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
