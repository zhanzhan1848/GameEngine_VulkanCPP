#include "ScreenProbeGIPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
// MetalDevice.h intentionally excluded — causes Rect naming conflict with MacTypes.h
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/Field/FieldView.h"
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

    // T4.6.3: Lumen Screen Probes deferred on Vulkan — CreateDescriptorSetLayouts()
    // uses Metal's overlapping texture/buffer binding idiom (rejected by Vulkan),
    // and no SPIR-V ports of the Screen Probe shaders exist yet. See
    // LumenDDGIPass::Initialize for the full rationale.
    if (device && device->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        std::cerr << "[ScreenProbeGI] Skipped on Vulkan (deferred — needs SPIR-V ports + "
                     "non-overlapping descriptor bindings)" << std::endl;
        return false;
    }

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

    // --- Trace: 5 textures (3 SDF + prev_color + surface_cache_lighting) + 3 SSBO (positions, normals, radiance) + 2 SSBO (card_data, card_lookup) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures (SDF cascades + prev frame color + surface cache lighting atlas)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 2
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev frame color
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface cache lighting atlas
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (write)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card data
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card lookup
        };
        DescriptorSetLayoutDesc layoutDesc{11, bindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- SDF Trace: 3 textures (SDF) + 2 SSBO (positions, normals) + 1 SSBO (hit_distance out) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 2
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_distance_buffer (out)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
        };
        DescriptorSetLayoutDesc layoutDesc{7, bindings};
        sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Finalize: 2 textures (prev_color + surface_cache_lighting) + 4 SSBO + 1 UBO + 2 SSBO (cards) ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev frame color
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface cache lighting atlas
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_distance_buffer (in)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (out)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card data
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card lookup
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        finalize_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Gather: 2 textures (depth, normal) + 1 storage image (output) + 2 SSBO + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output texture
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe SH (read, filtered)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
        };
        DescriptorSetLayoutDesc layoutDesc{7, bindings};
        gather_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Average: 3 SSBO (rayRadiance in, probeSH out, probeNormals in) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe SH output (write)
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // totalProbes + raysPerProbe
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read, for ray direction)
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        avg_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Temporal: 7 SSBO + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current avg radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history avg radiance (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current positions (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history positions (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // output radiance (write)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // temporal constants
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current normals (read)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history normals (read)
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        temporal_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Spatial Filter: 3 SSBO (inputSH, outputSH, positions) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // input SH (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // output SH (write)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // spatial constants
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        spatial_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Denoise: 3 sampled textures + 1 storage image + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // GI input (from Gather)
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // GI output (filtered)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // denoise params
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        denoise_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
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
    auto sdfTraceShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_sdf");
    auto finalizeShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_finalize");
    auto avgShader = CompileShader("ScreenProbeAverage", "screen_probe_average");
    if (avgShader == handles::INVALID_SHADER) {
        std::cerr << "[ScreenProbeGI] Average shader compilation failed" << std::endl;
        return;
    }
    auto gatherShader = CompileShader("ScreenProbeGather", "screen_probe_gather");
    auto temporalShader = CompileShader("ScreenProbeTemporal", "screen_probe_temporal");
    auto spatialShader = CompileShader("ScreenProbeSpatialFilter", "screen_probe_spatial_filter");
    auto denoiseShader = CompileShader("ScreenProbeDenoise", "screen_probe_denoise");

    if (placeShader == handles::INVALID_SHADER ||
        traceShader == handles::INVALID_SHADER ||
        sdfTraceShader == handles::INVALID_SHADER ||
        finalizeShader == handles::INVALID_SHADER ||
        gatherShader == handles::INVALID_SHADER ||
        temporalShader == handles::INVALID_SHADER ||
        spatialShader == handles::INVALID_SHADER ||
        denoiseShader == handles::INVALID_SHADER) {
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
        plDesc.setLayouts = &sdf_trace_set_layout_;
        sdf_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &finalize_set_layout_;
        finalize_layout_ = device_->CreatePipelineLayout(plDesc);
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
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &spatial_set_layout_;
        spatial_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &denoise_set_layout_;
        denoise_layout_ = device_->CreatePipelineLayout(plDesc);
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
        pipeDesc.computeShader = sdfTraceShader;
        pipeDesc.layout = sdf_trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = finalizeShader;
        pipeDesc.layout = finalize_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        finalize_pipeline_ = device_->CreateComputePipeline(pipeDesc);
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
        pipeDesc.threadGroupSize = {8, 8, 1};  // 2D for threadgroup probe caching
        gather_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = temporalShader;
        pipeDesc.layout = temporal_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        temporal_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = spatialShader;
        pipeDesc.layout = spatial_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        spatial_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = denoiseShader;
        pipeDesc.layout = denoise_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};  // 2D dispatch for screen-space filter
        denoise_pipeline_ = device_->CreateComputePipeline(pipeDesc);
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
            DescriptorSetDesc dsDesc{sdf_trace_set_layout_};
            sdf_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{finalize_set_layout_};
            finalize_ds_[i] = device_->CreateDescriptorSet(dsDesc);
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
        {
            DescriptorSetDesc dsDesc{spatial_set_layout_};
            spatial_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{denoise_set_layout_};
            denoise_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void ScreenProbeGIPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 640;  // ScreenProbeGlobalData padded (expanded for surface_cache_params)
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

    // Hit distance buffer (single, GPU-only intermediate for split trace)
    {
        u64 hitDistSize = (u64)totalProbes * params_.rays_per_probe * sizeof(float);
        BufferDesc desc{};
        desc.size = hitDistSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(float);
        hit_distance_buffer_ = device_->CreateBuffer(desc);
    }

    // Per-probe SH coefficient buffers (triple-buffered, 4 float4 per probe for L0+L1)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * 4 * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 4;
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
        desc.size = 32;  // totalProbes + alpha + posThreshold + normalThreshold + clampScale + pad[3]
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        temporal_cb_[i] = device_->CreateBuffer(desc);
    }

    // Spatial filter constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 16;  // totalProbes(u32) + gridW(u32) + sigma(f32) + pad(f32)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        spatial_cb_[i] = device_->CreateBuffer(desc);
    }

    // Denoise constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 32;  // sigma_depth(f32) + sigma_normal(f32) + sigma_spatial(f32) + kernel_radius(u32) + width(u32) + height(u32) + pad(u32) + pad(u32)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        denoise_cb_[i] = device_->CreateBuffer(desc);
    }

    // Output texture: full-resolution RGBA16_Float (raw Gather output)
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        output_texture_ = device_->CreateTexture(desc);
    }

    // Filtered output texture: full-resolution RGBA16_Float (Denoise output)
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        output_texture_filtered_ = device_->CreateTexture(desc);
    }

    // Dummy buffer for Metal validation when surface cache is unavailable
    {
        BufferDesc desc{};
        desc.size = 64;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = 4;
        desc.structured.elementStride = 16;
        dummy_buffer_ = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(dummy_buffer_);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(dummy_buffer_);
        }
    }

    // Dummy texture2D for Metal validation when surface cache is unavailable
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource;
        dummy_texture_2d_ = device_->CreateTexture(desc);
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

    // Import persistent output textures into render graph
    auto giOutputHandle = graph.ImportResource("ScreenProbeGI_Output", output_texture_);
    auto giFilteredHandle = graph.ImportResource("ScreenProbeGI_Filtered", output_texture_filtered_);
    // Bypass denoise: returning giFilteredHandle produced solid-color output in
    // fusion composite (mode 6) while mode 5 (which reads output_texture_ via
    // GetOutputTexture()) looked correct. Until denoise is debugged, route
    // downstream passes through the raw Gather texture for parity with mode 5.
    output.gi_output = giOutputHandle;

    // Import surface cache resources into render graph (needed for RGResourceHandle conversion)
    RGResourceHandle scLightingHandle;
    RGResourceHandle scCardDataHandle;
    RGResourceHandle scCardLookupHandle;
    if (surface_cache_available_) {
        scLightingHandle = graph.ImportResource("SurfaceCache_Lighting_ForProbes", surface_cache_lighting_atlas_);
        scCardDataHandle = graph.ImportResource("SurfaceCache_CardData_ForProbes", surface_cache_card_data_buffer_);
        scCardLookupHandle = graph.ImportResource("SurfaceCache_CardLookup_ForProbes", surface_cache_card_lookup_buffer_);
    }

    graph.AddPass<ScreenProbePassData>("ScreenProbeGI",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [this, gbuffer_depth, gbuffer_normal, prev_frame_color, giOutputHandle, giFilteredHandle,
         scLightingHandle, scCardDataHandle, scCardLookupHandle](
            ScreenProbePassData& data, RenderGraphBuilder& builder) {
            // Read GBuffer inputs
            builder.Read(gbuffer_depth, ResourceState::ShaderResource);
            builder.Read(gbuffer_normal, ResourceState::ShaderResource);

            // Read previous frame color for radiance sampling
            if (prev_frame_color.IsValid()) {
                builder.Read(prev_frame_color, ResourceState::ShaderResource);
            }

            // Read surface cache resources for near-hit sampling
            if (surface_cache_available_) {
                builder.Read(scLightingHandle, ResourceState::ShaderResource);
                builder.Read(scCardDataHandle, ResourceState::ShaderResource);
                builder.Read(scCardLookupHandle, ResourceState::ShaderResource);
            }

            // Write to raw output texture (Gather output)
            builder.Write(giOutputHandle, ResourceState::UnorderedAccess);
            // Write to filtered output texture (Denoise output)
            builder.Write(giFilteredHandle, ResourceState::UnorderedAccess);

            data.gbuffer_depth = gbuffer_depth;
            data.gbuffer_normal = gbuffer_normal;
            data.prev_frame_color = prev_frame_color;
            data.gi_output = giOutputHandle;
        },

        // ====================================================================
        // Execute lambda: dispatch 3 compute sub-passes
        // ====================================================================
        [this, camera_data, frameIdx, histIdx, giFilteredHandle](
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
            ResourceHandle filteredTex = ResolveTexture(giFilteredHandle);

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
            if (dbgSpFrame < 20) {
                std::cout << "[ScreenProbeGI] Frame " << dbgSpFrame
                          << " sdf=" << (sdfAvailable ? "YES" : "NO")
                          << " prevColor=" << prevColorTex
                          << " scAvail=" << (surface_cache_available_ ? "YES" : "NO")
                          << " depth=" << depthTex
                          << " normal=" << normalTex
                          << " outputTex(RG)=" << outputTex
                          << " output_texture_(member)=" << output_texture_
                          << " filteredTex=" << filteredTex
                          << " output_texture_filtered_(member)=" << output_texture_filtered_
                          << " denoise_pipe=" << denoise_pipeline_
                          << " place_pipe=" << place_pipeline_
                          << " trace_pipe=" << trace_pipeline_
                          << " gather_pipe=" << gather_pipeline_
                          << " grid=" << grid_width_ << "x" << grid_height_
                          << std::endl;
                dbgSpFrame++;
            }

            // ---- Prepare constant buffer data ----
            // Use SetComputeBytes (setBytes) instead of pool-allocated buffer
            // to bypass triple-buffer synchronization issues with memory pool
            ScreenProbeGlobalData gd{};
            {
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

                if (sdfAvailable) {
                    field::FieldDescriptor sdf_cascades[4];
                    u32 cascadeCount = field::FieldRegistry::Get().FindCascaded(
                        field::FieldSemantic::GlobalSDF, sdf_cascades, 4);
                    if (cascadeCount > 0) {
                        field::FieldView::WriteSDFToScreenProbe(sdf_cascades, cascadeCount, gd);
                    }
                }

                gd.surface_cache_params = {
                    surface_cache_available_ ? static_cast<float>(surface_cache_atlas_size_) : 0.0f,
                    static_cast<float>(surface_cache_card_count_),
                    surface_cache_available_ ? 1.0f : 0.0f,
                    0.0f
                };
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
                cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
            }

            // Barrier: probe buffers written
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
                ResourceHandle scLightingAtlas = surface_cache_available_ ? surface_cache_lighting_atlas_ : dummy_texture_2d_;
                ResourceHandle scCardData = surface_cache_available_ ? surface_cache_card_data_buffer_ : dummy_buffer_;
                ResourceHandle scCardLookup = surface_cache_available_ ? surface_cache_card_lookup_buffer_ : dummy_buffer_;

                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  sdfTextures[0]},
                    {1, DescriptorType::SampledImage,  sdfTextures[1]},
                    {2, DescriptorType::SampledImage,  sdfTextures[2]},
                    {3, DescriptorType::SampledImage,  prevColorTex},
                    {4, DescriptorType::SampledImage,  scLightingAtlas},
                    {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {4, DescriptorType::StorageBuffer, scCardData},
                    {5, DescriptorType::StorageBuffer, scCardLookup},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], params, 11);

                cmd->BindComputePipeline(trace_pipeline_);
                const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));

                u32 totalRays = grid_width_ * grid_height_ * params_.rays_per_probe;
                cmd->Dispatch((totalRays + 63) / 64, 1, 1);
            }

            // Barrier: radiance buffer written
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_radiance_buffer_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.5: ScreenProbeAverage (SH2 accumulation)
            // ==================================================================
            {
                struct AvgCB { u32 totalProbes; u32 raysPerProbe; float pad[2]; };
                AvgCB acb{grid_width_ * grid_height_, params_.rays_per_probe, 0.0f, 0.0f};

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {2, DescriptorType::UniformBuffer, avg_constants_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                };
                UpdateDescriptorSet(device_, avg_ds_[frameIdx], params, 4);

                cmd->BindComputePipeline(avg_pipeline_);
                const DescriptorSetHandle sets[] = { avg_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, avg_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(2, &acb, sizeof(AvgCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch(totalProbes, 1, 1);
            }

            // Barrier: avg radiance written
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
                struct TemporalCB { u32 totalProbes; float alpha; float posThreshold; float normalThreshold; float clampScale; float maxGradient; u32 pad[2]; };
                TemporalCB tcb{grid_width_ * grid_height_, 0.1f, 0.5f, 0.5f, 2.0f, 2.0f, {0, 0}};

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},
                    {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_positions_buffer_[histIdx]},
                    {4, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {5, DescriptorType::UniformBuffer, temporal_cb_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {7, DescriptorType::StorageBuffer, probe_normals_buffer_[histIdx]},
                };
                UpdateDescriptorSet(device_, temporal_ds_[frameIdx], params, 8);

                cmd->BindComputePipeline(temporal_pipeline_);
                const DescriptorSetHandle sets[] = { temporal_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, temporal_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(5, &tcb, sizeof(TemporalCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
            }

            // Barrier: temporal output written
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_avg_radiance_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.9: Probe Spatial Filter
            // ==================================================================
            if (spatial_pipeline_ != handles::INVALID_PIPELINE) {
                struct SpatialCB { u32 totalProbes; u32 gridW; float sigma; float pad; };
                SpatialCB scb{grid_width_ * grid_height_, grid_width_, 2.0f, 0.0f};

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},
                    {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, spatial_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, spatial_ds_[frameIdx], params, 4);

                cmd->BindComputePipeline(spatial_pipeline_);
                const DescriptorSetHandle sets[] = { spatial_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, spatial_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(3, &scb, sizeof(SpatialCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
            }

            // Barrier: spatial filter output written (histIdx)
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_avg_radiance_[histIdx];
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
                    {2, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},
                    {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {4, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                };
                UpdateDescriptorSet(device_, gather_ds_[frameIdx], params, 7);

                cmd->BindComputePipeline(gather_pipeline_);
                const DescriptorSetHandle sets[] = { gather_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, gather_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));

                u32 dispatchX = (render_width_ + 7) / 8;
                u32 dispatchY = (render_height_ + 7) / 8;
                cmd->Dispatch(dispatchX, dispatchY, 1);
            }

            // Barrier: Gather output written
            {
                ResourceBarrier barrier{};
                barrier.resource = outputTex;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 4: ScreenProbeDenoise
            // ==================================================================
            if (denoise_pipeline_ != handles::INVALID_PIPELINE &&
                filteredTex != handles::INVALID_RESOURCE) {
                struct DenoiseCB {
                    float sigma_depth; float sigma_normal; float sigma_spatial; float pad1;
                    u32 kernel_radius; u32 render_width; u32 render_height; u32 pad2;
                };
                DenoiseCB dcb{8.0f, 8.0f, 1.2f, 0.0f, 1, render_width_, render_height_, 0};

                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  outputTex},
                    {1, DescriptorType::SampledImage,  normalTex},
                    {2, DescriptorType::SampledImage,  depthTex},
                    {3, DescriptorType::StorageImage,  filteredTex},
                    {0, DescriptorType::UniformBuffer, denoise_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, denoise_ds_[frameIdx], params, 5);

                cmd->BindComputePipeline(denoise_pipeline_);
                const DescriptorSetHandle sets[] = { denoise_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, denoise_layout_, 0, 1, sets, 0, nullptr);
                cmd->SetComputeBytes(0, &dcb, sizeof(DenoiseCB));

                u32 dispatchX = (render_width_ + 7) / 8;
                u32 dispatchY = (render_height_ + 7) / 8;
                cmd->Dispatch(dispatchX, dispatchY, 1);
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
