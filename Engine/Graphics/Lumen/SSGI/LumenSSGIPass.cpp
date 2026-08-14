#include "LumenSSGIPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>

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
                   params[i].type == DescriptorType::SampledDepthImage ||
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
static const std::string LUMEN_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

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
                        std::cerr << "[LumenSSGI] Warning: Failed to read include: " << fullPath << std::endl;
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

static std::vector<u8> LoadShaderBytecode(rhi::RHIDeviceBase* device, const char* shaderName) {
    auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;

    if (platform == rhi::RHIPlatform::Vulkan) {
        // All SSGI shaders are hand-written GLSL compiled to .comp.spv in Lumen/.
        std::string relPath = utils::ShaderRegistry::GetShaderBaseDir(platform) +
                              "Lumen/" + shaderName + ".comp.spv";
        const std::vector<std::string> candidates = {
            relPath,
            "Engine/Graphics/Vulkan/shaders/Lumen/" + std::string(shaderName) + ".comp.spv",
        };
        for (const auto& path : candidates) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<u8> bytecode(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
                std::cerr << "[LumenSSGI] Failed to read SPIR-V: " << path << std::endl;
                return {};
            }
            return bytecode;
        }
        std::cerr << "[LumenSSGI] Failed to load SPIR-V: " << shaderName << std::endl;
        return {};
    }

    // Metal path (unchanged)
    std::string shaderPath = LUMEN_SHADER_DIR + shaderName + ".metal";

    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        std::cerr << "[LumenSSGI] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    // Resolve all #include "..." directives by inlining
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, LUMEN_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct LumenSSGIData {
    RGResourceHandle ssgi_trace;
    RGResourceHandle ssgi_temporal;
    RGResourceHandle ssgi_temporal_hist;
    RGResourceHandle ssgi_output;
};

} // anonymous namespace

// ============================================================================
// LumenSSGIPass Implementation
// ============================================================================

LumenSSGIPass::~LumenSSGIPass() {
    Shutdown();
}

bool LumenSSGIPass::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                               const SSGIParams& params) {
    if (initialized_) return true;

    // Phase 3: SSGI now active on Vulkan. WGSL shaders compile cleanly via naga
    // (no spvUnsafeArray issue). Descriptor layouts use flat bindings matching WGSL.

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    params_ = params;

    u32 half_w = render_width / 2;
    u32 half_h = render_height / 2;

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create compute pipelines + descriptor sets
    CreatePipelines();

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create persistent output textures
    CreatePersistentTextures();

    initialized_ = true;

    std::cout << "[LumenSSGI] Initialized (" << render_width << "x" << render_height
              << ", half-res: " << half_w << "x" << half_h << ")" << std::endl;
    return true;
}

void LumenSSGIPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    device_ = nullptr;

    // No explicit GPU resource destruction needed — handles are POD types
    // managed by the RHI device's garbage collector.
}

// ============================================================================
// Private helper methods
// ============================================================================

void LumenSSGIPass::CreateDescriptorSetLayouts() {
    // --- Descriptor layouts: platform-branched ---
    // Metal: overlapping texture/buffer namespaces.
    // Vulkan/Dawn: flat sequential bindings matching WGSL @binding(N).
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;

    // --- Trace ---
    if (isVk) {
        // GLSL SSGITrace.comp: 0=normal, 1=depth, 2=hzb, 3=prevColor, 4=output, 5=GlobalShaderData, 6=SSGIParams
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
        };
        trace_set_layout_ = device_->CreateDescriptorSetLayout({7, b});
    } else {
        DescriptorSetLayoutBinding traceBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        trace_set_layout_ = device_->CreateDescriptorSetLayout({7, traceBindings});
    }

    // --- Temporal ---
    if (isVk) {
        // WGSL SSGITemporal: 0=spatial, 1=history, 2=velocity, 3=depth, 4=output(storage), 5=params
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
        };
        temporal_set_layout_ = device_->CreateDescriptorSetLayout({6, b});
    } else {
        DescriptorSetLayoutBinding temporalBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        temporal_set_layout_ = device_->CreateDescriptorSetLayout({8, temporalBindings});
    }

    // --- Filter ---
    if (isVk) {
        // WGSL SSGIFilter: 0=input(half-res), 1=depth, 2=output(storage), 3=params
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
        };
        filter_set_layout_ = device_->CreateDescriptorSetLayout({4, b});
    } else {
        DescriptorSetLayoutBinding filterBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        filter_set_layout_ = device_->CreateDescriptorSetLayout({6, filterBindings});
    }

    // --- Half-res Denoise ---
    if (isVk) {
        // WGSL SSGIHalfResDenoise: 0=input, 1=output(storage), 2=params
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        halfres_denoise_set_layout_ = device_->CreateDescriptorSetLayout({3, b});
    } else {
        DescriptorSetLayoutBinding denoiseBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        halfres_denoise_set_layout_ = device_->CreateDescriptorSetLayout({3, denoiseBindings});
    }
}

void LumenSSGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(device_, name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    // Compile shaders
    auto traceShader = CompileShader("SSGITrace", "ssgi_trace");
    auto temporalShader = CompileShader("SSGITemporal", "ssgi_temporal");
    auto filterShader = CompileShader("SSGIFilter", "ssgi_filter");
    auto halfresDenoiseShader = CompileShader("SSGIHalfResDenoise", "ssgi_halfres_denoise");

    if (traceShader == handles::INVALID_SHADER ||
        temporalShader == handles::INVALID_SHADER ||
        filterShader == handles::INVALID_SHADER ||
        halfresDenoiseShader == handles::INVALID_SHADER) {
        std::cerr << "[LumenSSGI] Shader compilation failed" << std::endl;
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
        plDesc.setLayouts = &temporal_set_layout_;
        temporal_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &filter_set_layout_;
        filter_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &halfres_denoise_set_layout_;
        halfres_denoise_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = temporalShader;
        pipeDesc.layout = temporal_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        temporal_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = filterShader;
        pipeDesc.layout = filter_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        filter_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = halfresDenoiseShader;
        pipeDesc.layout = halfres_denoise_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        halfres_denoise_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{trace_set_layout_};
            trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{temporal_set_layout_};
            temporal_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{filter_set_layout_};
            filter_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{halfres_denoise_set_layout_};
            halfres_denoise_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void LumenSSGIPass::CreateConstantBuffers() {
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
    CreateCBs(params_cb_, 48);           // SSGIParams (Metal/GLSL layout, no matrices)
    CreateCBs(temporal_params_cb_, 32);  // SSGITemporalParams (16B + pad)
    CreateCBs(filter_params_cb_, 32);    // FilterParams (Metal layout)
    CreateCBs(halfres_denoise_cb_, 32);  // HalfResDenoiseParams (16 bytes + padding)
}

void LumenSSGIPass::CreatePersistentTextures() {
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    // Half-res trace output
    {
        TextureDesc desc{};
        desc.size = {half_w, half_h, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        trace_texture_ = device_->CreateTexture(desc);
    }

    // Half-res denoised trace output
    {
        TextureDesc desc{};
        desc.size = {half_w, half_h, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        trace_denoised_texture_ = device_->CreateTexture(desc);
    }

    // Full-res temporal textures (triple-buffered)
    for (int i = 0; i < 3; i++) {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        temporal_textures_[i] = device_->CreateTexture(desc);
    }

    // Full-res filter output
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        filter_texture_ = device_->CreateTexture(desc);
    }
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

LumenSSGIOutput LumenSSGIPass::AddPass(
    RenderGraph& graph,
    RGResourceHandle gbuffer_normal,
    RGResourceHandle gbuffer_depth,
    RGResourceHandle gbuffer_velocity,
    RGResourceHandle hzb_texture,
    RGResourceHandle prev_frame_color,
    const SSGICameraData& camera_data,
    u32 current_frame_index,
    u32 hzb_mip_levels)
{
    LumenSSGIOutput output{};

    // Import persistent textures into render graph
    auto traceHandle = graph.ImportResource("LumenSSGI_Trace", trace_texture_);
    auto filterHandle = graph.ImportResource("LumenSSGI_Filter", filter_texture_);

    // Triple-buffered temporal indexing
    u32 outIdx = current_frame_index % 3;
    u32 histIdx = (current_frame_index + 2) % 3;

    // Import temporal textures
    auto temporalOutHandle = graph.ImportResource(
        "LumenSSGI_Temporal_" + std::to_string(outIdx), temporal_textures_[outIdx]);
    auto temporalHistHandle = graph.ImportResource(
        "LumenSSGI_TemporalHist_" + std::to_string(histIdx), temporal_textures_[histIdx]);

    // Store output handle — the temporally accumulated texture is the final
    // SSGI result on all backends (trace → denoise → filter → temporal).
    output.ssgi_output = temporalOutHandle;

    static int s_out = 0;
    if (s_out < 3) {
        std::cerr << "[SSGI_OUT] temporalOutHandle_valid=" << temporalOutHandle.IsValid()
                  << " output_valid=" << output.ssgi_output.IsValid()
                  << " trace_texture=" << trace_texture_ << std::endl;
        s_out++;
    }

    graph.AddPass<LumenSSGIData>("LumenSSGI",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [gbuffer_normal, gbuffer_depth, gbuffer_velocity, hzb_texture,
         prev_frame_color, traceHandle, temporalOutHandle,
         temporalHistHandle, filterHandle](
            LumenSSGIData& data, RenderGraphBuilder& builder) {
            // Read all input textures
            builder.SideEffect();
            builder.Read(gbuffer_normal,   ResourceState::ShaderResource);
            builder.Read(gbuffer_depth,    ResourceState::ShaderResource);
            builder.Read(gbuffer_velocity, ResourceState::ShaderResource);
            builder.Read(hzb_texture,      ResourceState::ShaderResource);
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            // Write to SSGI output textures
            data.ssgi_trace = builder.Write(traceHandle, ResourceState::UnorderedAccess);
            data.ssgi_temporal = builder.Write(temporalOutHandle, ResourceState::UnorderedAccess);
            data.ssgi_temporal_hist = builder.Read(temporalHistHandle, ResourceState::ShaderResource);
            data.ssgi_output = builder.Write(filterHandle, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: dispatch 3 compute sub-passes
        // ====================================================================
        [this, camera_data, current_frame_index, hzb_mip_levels,
         gbuffer_normal, gbuffer_depth, gbuffer_velocity,
         hzb_texture, prev_frame_color, outIdx, histIdx](
            const LumenSSGIData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            static int s_ssgi_exec = 0;
            if (s_ssgi_exec < 3) { std::cerr << "[SSGI_EXEC] frame " << s_ssgi_exec
                << " trace=" << (trace_pipeline_ != handles::INVALID_PIPELINE ? "OK" : "INVALID")
                << " filter=" << (filter_pipeline_ != handles::INVALID_PIPELINE ? "OK" : "INVALID")
                << " temporal=" << (temporal_pipeline_ != handles::INVALID_PIPELINE ? "OK" : "INVALID")
                << std::endl; s_ssgi_exec++; }
            if (!cmd) return;

            u32 frameIdx = current_frame_index % 3;
            u32 half_w = render_width_ / 2;
            u32 half_h = render_height_ / 2;
            constexpr u32 TG = 8;

            // Resolve physical handles from render graph
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle normalTex     = ResolveTexture(gbuffer_normal);
            ResourceHandle depthTex      = ResolveTexture(gbuffer_depth);
            ResourceHandle velocityTex   = ResolveTexture(gbuffer_velocity);
            ResourceHandle hzbTex        = ResolveTexture(hzb_texture);
            ResourceHandle prevColorTex  = ResolveTexture(prev_frame_color);

            // Force HZB texture to ShaderResource layout (all mips).
            // HZBSystem's per-mip transitions may leave some mips in UNDEFINED.
            if (hzbTex != handles::INVALID_RESOURCE) {
                ResourceBarrier hzbBar{};
                hzbBar.resource = hzbTex;
                hzbBar.beforeState = ResourceState::ShaderResource;
                hzbBar.afterState = ResourceState::ShaderResource;
                hzbBar.subresource = 0xFFFFFFFF;
                hzbBar.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&hzbBar, 1);
            }

            // DEBUG: Log SSGI dimensions and resource validity (every 120 frames)
            static u32 dbgFrame = 0;
            if (dbgFrame < 3) {
                std::cout << "[LumenSSGI] Frame " << dbgFrame
                          << " render=" << render_width_ << "x" << render_height_
                          << " half=" << half_w << "x" << half_h
                          << " frameIdx=" << frameIdx
                          << "\n  normalTex=" << normalTex
                          << " depthTex=" << depthTex
                          << " velocityTex=" << velocityTex
                          << " hzbTex=" << hzbTex
                          << " prevColorTex=" << prevColorTex
                          << "\n  trace_tex=" << trace_texture_
                          << " temporal_out=" << temporal_textures_[outIdx]
                          << " temporal_hist=" << temporal_textures_[histIdx]
                          << " filter_tex=" << filter_texture_
                          << std::endl;
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
                    mapped->ViewProjection = camera_data.view_matrix * camera_data.proj_matrix;
                    mapped->PreviousViewProjection = camera_data.prev_view_matrix * camera_data.prev_proj_matrix;
                    mapped->InvViewProjection = rhi::math::Inverse(mapped->ViewProjection);

                    math::m4x4 invView = rhi::math::Inverse(camera_data.view_matrix);
                    mapped->CameraPositionAndViewWidth = {
                        invView.columns[3][0], invView.columns[3][1],
                        invView.columns[3][2], (float)render_width_
                    };

                    auto& vm = camera_data.view_matrix;
                    mapped->CameraDirectionAndViewHeight = {
                        -vm.columns[2][0], -vm.columns[2][1],
                        -vm.columns[2][2], (float)render_height_
                    };

                    mapped->NumDirectionalLights = 0;
                    mapped->DeltaTime = camera_data.delta_time;
                    mapped->FrameCount = (float)camera_data.frame_index;
                    device_->UnmapBuffer(global_cb_[frameIdx]);
                }
            }

            // ================================================================
            // Sub-pass 1: Trace (half-res)
            // ================================================================
            if (trace_pipeline_ != handles::INVALID_PIPELINE &&
                trace_texture_ != handles::INVALID_RESOURCE) {
                // Upload SSGIParams
                // SSGIParams: matches Metal/GLSL layout (no matrices — from GlobalShaderData)
                struct SSGIParamsData {
                    u32   ray_count;
                    float radius;
                    float thickness;
                    u32   frame_index;
                    u32   output_width;
                    u32   output_height;
                    float near_plane;
                    float far_plane;
                    u32   hzb_mip_levels;
                    float max_trace_distance;
                };
                auto* ssgiParams = static_cast<SSGIParamsData*>(device_->MapBuffer(params_cb_[frameIdx]));
                if (ssgiParams) {
                    ssgiParams->ray_count = params_.ray_count;
                    ssgiParams->radius = params_.radius;
                    ssgiParams->thickness = params_.thickness;
                    ssgiParams->frame_index = camera_data.frame_index;
                    ssgiParams->output_width = half_w;
                    ssgiParams->output_height = half_h;
                    ssgiParams->near_plane = 0.1f;
                    ssgiParams->far_plane = 1000.0f;
                    ssgiParams->hzb_mip_levels = hzb_mip_levels;
                    ssgiParams->max_trace_distance = params_.max_trace_distance;
                    device_->UnmapBuffer(params_cb_[frameIdx]);
                }

                // Update descriptor set
                if (isVk) {
                    // GLSL flat: 0=normal, 1=depth, 2=hzb, 3=prevColor, 4=output, 5=GlobalShaderData, 6=SSGIParams
                    DescriptorData traceParams[] = {
                        {0, DescriptorType::SampledImage,      normalTex},
                        {1, DescriptorType::SampledDepthImage, depthTex},
                        {2, DescriptorType::SampledImage,      hzbTex},
                        {3, DescriptorType::SampledImage,      prevColorTex},
                        {4, DescriptorType::StorageImage,      trace_texture_},
                        {5, DescriptorType::UniformBuffer,     global_cb_[frameIdx]},
                        {6, DescriptorType::UniformBuffer,     params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 7);
                } else {
                    DescriptorData traceParams[] = {
                        {0, DescriptorType::SampledImage,  normalTex},
                        {1, DescriptorType::SampledImage,  depthTex},
                        {2, DescriptorType::SampledImage,  hzbTex},
                        {3, DescriptorType::SampledImage,  prevColorTex},
                        {4, DescriptorType::StorageImage,  trace_texture_},
                        {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                        {1, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 7);
                }

                cmd->BindComputePipeline(trace_pipeline_);
                const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);

                u32 gx = (half_w + TG - 1) / TG;
                u32 gy = (half_h + TG - 1) / TG;

                // DEBUG: Log first 3 frames
                static u32 traceFrame = 0;
                if (traceFrame < 3) {
                    std::cout << "[LumenSSGI] TRACE dispatch: gx=" << gx << " gy=" << gy
                              << " half=" << half_w << "x" << half_h
                              << " total_threads=" << (gx*TG) << "x" << (gy*TG)
                              << std::endl;
                    traceFrame++;
                }

                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: trace UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = trace_texture_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 1.5: Half-res Denoise (5x5 bilateral on trace output)
            // ================================================================
            bool denoiseRan = false;
            if (halfres_denoise_pipeline_ != handles::INVALID_PIPELINE &&
                trace_denoised_texture_ != handles::INVALID_RESOURCE) {
                struct HalfResDenoiseParamsData {
                    u32   width;
                    u32   height;
                    float sigma;
                    float pad[5];
                };
                auto* denoiseParams = static_cast<HalfResDenoiseParamsData*>(device_->MapBuffer(halfres_denoise_cb_[frameIdx]));
                if (denoiseParams) {
                    denoiseParams->width = half_w;
                    denoiseParams->height = half_h;
                    denoiseParams->sigma = 1.5f;
                    denoiseParams->pad[0] = 0.0f;
                    denoiseParams->pad[1] = 0.0f;
                    denoiseParams->pad[2] = 0.0f;
                    denoiseParams->pad[3] = 0.0f;
                    denoiseParams->pad[4] = 0.0f;
                    device_->UnmapBuffer(halfres_denoise_cb_[frameIdx]);
                }

                if (isVk) {
                    DescriptorData d[] = {
                        {0, DescriptorType::SampledImage,  trace_texture_},
                        {1, DescriptorType::StorageImage,  trace_denoised_texture_},
                        {2, DescriptorType::UniformBuffer, halfres_denoise_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, halfres_denoise_ds_[frameIdx], d, 3);
                } else {
                    DescriptorData denoiseParamsDesc[] = {
                        {0, DescriptorType::SampledImage,  trace_texture_},
                        {1, DescriptorType::StorageImage,  trace_denoised_texture_},
                        {0, DescriptorType::UniformBuffer, halfres_denoise_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, halfres_denoise_ds_[frameIdx], denoiseParamsDesc, 3);
                }

                cmd->BindComputePipeline(halfres_denoise_pipeline_);
                const DescriptorSetHandle denoiseSets[] = { halfres_denoise_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, halfres_denoise_layout_, 0, 1, denoiseSets, 0, nullptr);

                u32 gx = (half_w + TG - 1) / TG;
                u32 gy = (half_h + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
                denoiseRan = true;
            }

            ResourceHandle filterInputTexture;
            if (denoiseRan) {
                ResourceBarrier barrier{};
                barrier.resource = trace_denoised_texture_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
                filterInputTexture = trace_denoised_texture_;
            } else {
                filterInputTexture = trace_texture_;
            }

            // ================================================================
            // Sub-pass 2: Spatial Filter (full-res, reads half-res trace)
            // ================================================================
            if (filter_pipeline_ != handles::INVALID_PIPELINE &&
                filter_texture_ != handles::INVALID_RESOURCE) {
                struct FilterParamsData {
                    float sigma_depth;
                    float sigma_normal;
                    float sigma_hit_dist;
                    float sigma_spatial;
                    u32   kernel_radius;
                };
                auto* filterParams = static_cast<FilterParamsData*>(device_->MapBuffer(filter_params_cb_[frameIdx]));
                if (filterParams) {
                    filterParams->sigma_depth = params_.filter_sigma_depth;
                    filterParams->sigma_normal = params_.filter_sigma_normal;
                    filterParams->sigma_hit_dist = params_.filter_sigma_hit_dist;
                    filterParams->sigma_spatial = params_.filter_sigma_spatial;
                    filterParams->kernel_radius = params_.filter_kernel_radius;
                    device_->UnmapBuffer(filter_params_cb_[frameIdx]);
                }

                if (isVk) {
                    // WGSL flat: 0=input, 1=depth, 2=output, 3=params
                    DescriptorData d[] = {
                        {0, DescriptorType::SampledImage,      filterInputTexture},
                        {1, DescriptorType::SampledDepthImage, depthTex},
                        {2, DescriptorType::StorageImage,      filter_texture_},
                        {3, DescriptorType::UniformBuffer,     filter_params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, filter_ds_[frameIdx], d, 4);
                } else {
                    DescriptorData filterParamsDesc[] = {
                        {0, DescriptorType::SampledImage,  filterInputTexture},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::SampledImage,  depthTex},
                        {3, DescriptorType::StorageImage,  filter_texture_},
                        {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                        {1, DescriptorType::UniformBuffer, filter_params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, filter_ds_[frameIdx], filterParamsDesc, 6);
                }

                cmd->BindComputePipeline(filter_pipeline_);
                const DescriptorSetHandle sets[] = { filter_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, filter_layout_, 0, 1, sets, 0, nullptr);

                u32 gx = (render_width_ + TG - 1) / TG;
                u32 gy = (render_height_ + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: filter UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = filter_texture_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 3: Temporal Accumulation (full-res)
            // ================================================================
            if (temporal_pipeline_ != handles::INVALID_PIPELINE &&
                temporal_textures_[outIdx] != handles::INVALID_RESOURCE) {
                struct TemporalParamsData {
                    float feedback;
                    u32   full_width;
                    u32   full_height;
                };
                auto* temporalParams = static_cast<TemporalParamsData*>(device_->MapBuffer(temporal_params_cb_[frameIdx]));
                if (temporalParams) {
                    temporalParams->feedback = params_.temporal_feedback;
                    temporalParams->full_width = render_width_;
                    temporalParams->full_height = render_height_;
                    device_->UnmapBuffer(temporal_params_cb_[frameIdx]);
                }

                if (isVk) {
                    // WGSL flat: 0=spatial, 1=history, 2=velocity, 3=depth, 4=output, 5=params
                    DescriptorData d[] = {
                        {0, DescriptorType::SampledImage,      filter_texture_},
                        {1, DescriptorType::SampledImage,      temporal_textures_[histIdx]},
                        {2, DescriptorType::SampledImage,      velocityTex},
                        {3, DescriptorType::SampledDepthImage, depthTex},
                        {4, DescriptorType::StorageImage,      temporal_textures_[outIdx]},
                        {5, DescriptorType::UniformBuffer,     temporal_params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, temporal_ds_[frameIdx], d, 6);
                } else {
                    DescriptorData temporalParamsDesc[] = {
                        {0, DescriptorType::SampledImage,  filter_texture_},
                        {1, DescriptorType::SampledImage,  temporal_textures_[histIdx]},
                        {2, DescriptorType::SampledImage,  velocityTex},
                        {3, DescriptorType::SampledImage,  depthTex},
                        {4, DescriptorType::SampledImage,  normalTex},
                        {5, DescriptorType::StorageImage,  temporal_textures_[outIdx]},
                        {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                        {1, DescriptorType::UniformBuffer, temporal_params_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, temporal_ds_[frameIdx], temporalParamsDesc, 8);
                }

                cmd->BindComputePipeline(temporal_pipeline_);
                const DescriptorSetHandle sets[] = { temporal_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, temporal_layout_, 0, 1, sets, 0, nullptr);

                u32 gx = (render_width_ + TG - 1) / TG;
                u32 gy = (render_height_ + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: temporal UAV -> SRV (final output)
            {
                ResourceBarrier barrier{};
                barrier.resource = temporal_textures_[outIdx];
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
