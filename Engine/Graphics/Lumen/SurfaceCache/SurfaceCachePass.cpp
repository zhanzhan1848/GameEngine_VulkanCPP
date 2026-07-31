#include "SurfaceCachePass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
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
// Descriptor update helper (same pattern as LumenDDGIPass)
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
// (Same pattern as LumenDDGIPass — Metal's newLibrary(source) can't resolve
// #include without include dirs, so we manually inline local includes.)
// ============================================================================

static const std::string SHADER_BASE_DIR = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
static const std::string SC_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

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
                    } else {
                        std::cerr << "[SurfaceCache] Warning: Failed to read include: " << fullPath << std::endl;
                        out << line << "\n";
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
    std::string shaderPath = SC_SHADER_DIR + shaderName + ".metal";

    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        std::cerr << "[SurfaceCache] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    // Resolve all #include "..." directives by inlining
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, SC_SHADER_DIR, included);

    // DEBUG: Dump resolved source to file
    {
        std::string dumpPath = "/tmp/sc_" + std::string(shaderName) + ".resolved.metal";
        std::ofstream dumpFile(dumpPath);
        if (dumpFile.is_open()) {
            dumpFile << resolved;
            dumpFile.close();
            std::cerr << "[SurfaceCache] Dumped resolved source to " << dumpPath
                      << " (" << resolved.size() << " bytes)" << std::endl;
        }
    }

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct SurfaceCacheRGData {
    RGResourceHandle prev_frame_color;
};

} // anonymous namespace

// ============================================================================
// SurfaceCachePass Implementation
// ============================================================================

SurfaceCachePass::~SurfaceCachePass() {
    Shutdown();
}

bool SurfaceCachePass::Initialize(RHIDeviceBase* device, const LumenConfig& config) {
    if (initialized_) return true;

    // T4.6.3: Lumen Surface Cache deferred on Vulkan — CreateDescriptorSetLayouts()
    // uses Metal's overlapping texture/buffer binding idiom (rejected by Vulkan),
    // and no SPIR-V ports of the Surface Cache shaders exist yet. See
    // LumenDDGIPass::Initialize for the full rationale.
    if (device && device->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        std::cerr << "[SurfaceCache] Skipped on Vulkan (deferred — needs SPIR-V ports + "
                     "non-overlapping descriptor bindings)" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;
    atlas_size_ = config.surface_cache_atlas_size;
    page_size_ = config.surface_cache_page_size;

    // Initialize card generator
    card_generator_.Initialize(device, atlas_size_, page_size_, config.surface_cache_max_cards);

    // Create atlas textures
    CreateAtlasTextures();

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create pipelines and triple-buffered descriptor sets
    CreatePipelines();

    initialized_ = true;

    std::cout << "[SurfaceCache] Initialized (atlas: "
              << atlas_size_ << "x" << atlas_size_
              << ", page: " << page_size_
              << ", max_cards: " << config.surface_cache_max_cards << ")" << std::endl;
    return true;
}

void SurfaceCachePass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    device_ = nullptr;

    // No explicit GPU resource destruction needed -- handles are POD types
    // managed by the RHI device's garbage collector.
}

// ============================================================================
// Private helper methods
// ============================================================================

void SurfaceCachePass::CreateAtlasTextures() {
    auto CreateAtlas = [&](const char* name, DataFormat format, ResourceHandle& handle, bool renderTarget = false) {
        TextureDesc desc{};
        desc.size = { atlas_size_, atlas_size_, 1 };
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = format;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        if (renderTarget) desc.usage = desc.usage | TextureUsage::RenderTarget;
        desc.name = name;
        handle = device_->CreateTexture(desc);
    };

    // Albedo: RGBA8 (albedo + alpha) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Albedo", DataFormat::RGBA8_UNorm, albedo_atlas_, true);

    // Normal: RG16F (octahedral-encoded normal) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Normal", DataFormat::RG16_Float, normal_atlas_, true);

    // Depth: R32F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Depth", DataFormat::R32_Float, depth_atlas_, true);

    // Emissive: RGBA16F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Emissive", DataFormat::RGBA16_Float, emissive_atlas_, true);

    // Lighting atlas: triple-buffered (RGBA16F for HDR lighting)
    for (int i = 0; i < 3; ++i) {
        CreateAtlas(
            ("SurfaceCache_Lighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            lighting_atlas_[i]);
        CreateAtlas(
            ("SurfaceCache_PrevLighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            prev_lighting_atlas_[i]);
    }

    // Runtime buffers
    {
        // Light assignment buffer: per-tile light index lists
        u32 pagesPerSide = atlas_size_ / page_size_;
        u32 totalPages = pagesPerSide * pagesPerSide;
        BufferDesc lightAssignDesc{};
        lightAssignDesc.size = (u64)totalPages * config_.surface_cache_max_cards * sizeof(u32);
        lightAssignDesc.type = BufferType::Structured;
        lightAssignDesc.usage = GPUMemoryUsage::Dynamic;
        lightAssignDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        light_assignment_buffer_ = device_->CreateBuffer(lightAssignDesc);

        // Initialize to all 0xFF (empty tiles) so LightEval skips safely
        // when LightCull hasn't written to some tiles.
        auto* mapped = static_cast<uint32_t*>(device_->MapBuffer(light_assignment_buffer_));
        if (mapped) {
            memset(mapped, 0xFF, lightAssignDesc.size);
            device_->UnmapBuffer(light_assignment_buffer_);
        }
    }

    {
        // Light info buffer: light parameters for evaluation
        BufferDesc lightInfoDesc{};
        lightInfoDesc.size = 256 * sizeof(u32) * 4;  // Max 256 lights, 64 bytes each
        lightInfoDesc.type = BufferType::Structured;
        lightInfoDesc.usage = GPUMemoryUsage::Dynamic;
        lightInfoDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        light_info_buffer_ = device_->CreateBuffer(lightInfoDesc);
    }

    {
        // Ray hits buffer: for indirect tracing
        u32 pagesPerSide = atlas_size_ / page_size_;
        u32 totalPages = pagesPerSide * pagesPerSide;
        BufferDesc rayHitsDesc{};
        rayHitsDesc.size = (u64)totalPages * page_size_ * page_size_ * sizeof(float) * 4; // RGBA per texel
        rayHitsDesc.type = BufferType::Structured;
        rayHitsDesc.usage = GPUMemoryUsage::Dynamic;
        rayHitsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ray_hits_buffer_ = device_->CreateBuffer(rayHitsDesc);
    }

    {
        // Card dispatch buffer: flattened per-card texel dispatch info
        BufferDesc desc{};
        desc.size = (u64)config_.surface_cache_max_cards * sizeof(CardDispatchInfo);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        card_dispatch_buffer_ = device_->CreateBuffer(desc);
    }
}

void SurfaceCachePass::CreateConstantBuffers() {
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

    CreateCBs(global_cb_, 512);    // GlobalShaderData (padded)
    CreateCBs(params_cb_, 512);    // SurfaceCacheParams (padded)
}

void SurfaceCachePass::CreateDescriptorSetLayouts() {
    // --- Dilate: 2 textures (depth_in read + depth_out write) + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_in  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_out [[texture(1)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params    [[buffer(1)]]
        };
        DescriptorSetLayoutDesc layoutDesc{3, bindings};
        dilate_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- LightCull: 2 textures (depth + normal atlas) + 1 UBO + 2 SSBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas [[texture(1)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params       [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights       [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile_assignment [[buffer(3)]]
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        light_cull_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- LightEval (merged): 4 textures + 1 UBO + 3 SSBO = 8 bindings ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // albedo_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas  [[texture(1)]]
            {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // emissive_atlas [[texture(2)]]
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // lighting_out  [[texture(3)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params        [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards         [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights        [[buffer(3)]]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch [[buffer(4)]]
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        light_eval_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Capture (graphics): 3 textures (material arrays) + 1 UBO + 1 SSBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // albedo_array  [[texture(0)]]
            {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // normal_array  [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // orm_array     [[texture(2)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel, nullptr}, // pass_data [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex, nullptr},   // instances     [[buffer(2)]]
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        capture_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectTrace: 5 textures (depth+normal+3 SDF) + 1 UBO + 1 UBO + 2 SSBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf0         [[texture(2)]]
            {3, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf1         [[texture(3)]]
            {4, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf2         [[texture(4)]]
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb    [[buffer(0)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params       [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards        [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits     [[buffer(3)]]
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectResolve: 4 textures + 1 UBO + 1 UBO + 3 SSBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // prev_lighting [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // albedo_atlas  [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sky_cubemap   [[texture(2)]]
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // indirect_out  [[texture(3)]]
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb     [[buffer(0)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params        [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lookups       [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards         [[buffer(3)]]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits      [[buffer(4)]]
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        indirect_resolve_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void SurfaceCachePass::CreatePipelines() {
    // --- Shader compilation helper ---
    auto CompileComputeShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto CompileGraphicsShader = [&](const char* name, const char* entry, ShaderStage stage) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), stage, entry);
    };

    // --- Compile all compute shaders ---
    auto dilateShader       = CompileComputeShader("SurfaceCacheDilate", "surfaceCacheDilate");
    auto lightCullShader    = CompileComputeShader("SurfaceCacheLightCull", "surfaceCacheLightCull");
    auto lightEvalShader    = CompileComputeShader("SurfaceCacheLightEval", "surfaceCacheLightEval");
    auto indirectTraceShader  = CompileComputeShader("SurfaceCacheIndirectTrace", "surfaceCacheIndirectTrace");
    auto indirectResolveShader = CompileComputeShader("SurfaceCacheIndirectResolve", "surfaceCacheIndirectResolve");

    // --- Compile capture graphics shaders ---
    auto captureVS = CompileGraphicsShader("SurfaceCacheCapture", "surfaceCacheCaptureVS", ShaderStage::Vertex);
    auto captureFS = CompileGraphicsShader("SurfaceCacheCapture", "surfaceCacheCaptureFS", ShaderStage::Pixel);

    if (dilateShader == handles::INVALID_SHADER ||
        lightCullShader == handles::INVALID_SHADER ||
        lightEvalShader == handles::INVALID_SHADER ||
        indirectTraceShader == handles::INVALID_SHADER ||
        indirectResolveShader == handles::INVALID_SHADER ||
        captureVS == handles::INVALID_SHADER ||
        captureFS == handles::INVALID_SHADER) {
        std::cerr << "[SurfaceCache] Shader compilation failed" << std::endl;
        return;
    }

    // --- Create pipeline layouts ---
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &dilate_set_layout_;
        dilate_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &light_cull_set_layout_;
        light_cull_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &light_eval_set_layout_;
        light_eval_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &capture_set_layout_;
        capture_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &indirect_trace_set_layout_;
        indirect_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &indirect_resolve_set_layout_;
        indirect_resolve_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // --- Create compute pipelines ---
    // Dilate: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = dilateShader;
        pipeDesc.layout = dilate_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        dilate_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // LightCull: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightCullShader;
        pipeDesc.layout = light_cull_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        light_cull_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // LightEval (merged): (256,1,1) — 1D flattened card-texel dispatch
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightEvalShader;
        pipeDesc.layout = light_eval_layout_;
        pipeDesc.threadGroupSize = {256, 1, 1};
        light_eval_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // IndirectTrace: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = indirectTraceShader;
        pipeDesc.layout = indirect_trace_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        indirect_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // IndirectResolve: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = indirectResolveShader;
        pipeDesc.layout = indirect_resolve_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        indirect_resolve_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // --- Create capture graphics pipeline ---
    {
        GraphicsPipelineDesc desc{};
        desc.layout = capture_layout_;
        desc.vertexShader = captureVS;
        desc.pixelShader = captureFS;

        // Vertex attributes: position(0)=float3, normal(1)=float3, uv(2)=float2
        desc.vertexAttributes.clear();
        desc.vertexAttributes.push_back({0, 0, DataFormat::RGB32_Float,  0});  // position @ offset 0
        desc.vertexAttributes.push_back({1, 0, DataFormat::RGB32_Float, 12});  // normal   @ offset 12
        desc.vertexAttributes.push_back({2, 0, DataFormat::RG32_Float,  24});  // uv       @ offset 24

        desc.vertexBindings.clear();
        desc.vertexBindings.push_back({0, 32, true});  // binding 0, stride=32, per-vertex

        desc.topology = PrimitiveTopology::TriangleList;
        desc.cullMode = CullMode::None;

        // 4 render targets matching CaptureOutput struct in shader
        desc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;    // albedo  [[color(0)]]
        desc.renderTargetFormats[1] = DataFormat::RG16_Float;     // normal  [[color(1)]]
        desc.renderTargetFormats[2] = DataFormat::R32_Float;      // depth   [[color(2)]]
        desc.renderTargetFormats[3] = DataFormat::RGBA16_Float;   // emissive [[color(3)]]
        desc.renderTargetCount = 4;

        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;

        capture_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }

    // --- Create triple-buffered descriptor sets ---
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{dilate_set_layout_};
            dilate_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{light_cull_set_layout_};
            light_cull_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{light_eval_set_layout_};
            light_eval_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{capture_set_layout_};
            capture_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{indirect_trace_set_layout_};
            indirect_trace_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{indirect_resolve_set_layout_};
            indirect_resolve_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

SurfaceCacheOutput SurfaceCachePass::AddPass(
    RenderGraph& graph,
    RGResourceHandle prev_frame_color,
    ResourceHandle light_data_buffer,
    const SurfaceCacheFrameData& frame_data,
    u32 current_frame_index)
{
    SurfaceCacheOutput output{};

    u32 outIdx = current_frame_index % 3;

    // Import persistent lighting atlas into render graph
    auto lightingHandle = graph.ImportResource(
        "SurfaceCache_Lighting_" + std::to_string(outIdx),
        lighting_atlas_[outIdx]);

    output.lighting_atlas = lightingHandle;

    graph.AddPass<SurfaceCacheRGData>("LumenSurfaceCache",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, lightingHandle](
            SurfaceCacheRGData& data, RenderGraphBuilder& builder) {
            // Read previous frame color
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;

            // Write to lighting atlas
            builder.Write(lightingHandle, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: merged per-card texel lighting dispatch
        // ====================================================================
        [this, frame_data, current_frame_index, outIdx, lightingHandle](
            const SurfaceCacheRGData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            u32 frameIdx = current_frame_index % 3;

            // Guard: pipeline must be valid
            if (light_eval_pipeline_ == handles::INVALID_PIPELINE) return;

            // ================================================================
            // 1. Build CardDispatchInfo from active cards (prefix-sum)
            // ================================================================
            const auto& cards = card_generator_.GetCards();
            u32 card_count = card_generator_.GetCardCount();
            u32 total_texels = 0;

            std::vector<CardDispatchInfo> dispatch_info(card_count);
            for (u32 i = 0; i < card_count; ++i) {
                u32 res = cards[i].resolution;
                u32 count = res * res;
                dispatch_info[i].texel_offset = total_texels;
                dispatch_info[i].texel_count = count;
                dispatch_info[i].resolution = res;
                dispatch_info[i].atlas_offset_x = cards[i].atlas_offset_x;
                dispatch_info[i].atlas_offset_y = cards[i].atlas_offset_y;
                memset(dispatch_info[i]._pad, 0, sizeof(dispatch_info[i]._pad));
                total_texels += count;
            }

            // Skip dispatch when no card data
            if (total_texels == 0) return;

            // Upload dispatch info to GPU
            {
                auto* mapped = static_cast<CardDispatchInfo*>(device_->MapBuffer(card_dispatch_buffer_));
                if (mapped) {
                    memcpy(mapped, dispatch_info.data(), card_count * sizeof(CardDispatchInfo));
                    device_->UnmapBuffer(card_dispatch_buffer_);
                }
            }

            // ================================================================
            // 2. Upload FlattenedLightingParams constant buffer
            // ================================================================
            {
                auto* mapped = static_cast<FlattenedLightingParams*>(device_->MapBuffer(params_cb_[frameIdx]));
                if (mapped) {
                    FlattenedLightingParams params{};
                    params.sc_params.atlas_size = atlas_size_;
                    params.sc_params.page_size = page_size_;
                    params.sc_params.capture_budget_pages = config_.surface_cache_capture_budget;
                    params.sc_params.max_cards = config_.surface_cache_max_cards;
                    params.sc_params.update_distance = config_.surface_cache_update_distance;
                    params.sc_params.importance_weight = config_.surface_cache_importance_weight;
                    params.sc_params.max_lights_per_tile = 32;
                    params.sc_params.indirect_rays_per_probe = 64;
                    params.sc_params.indirect_temporal_weight = 0.9f;
                    params.sc_params.indirect_near_distance = 0.1f;
                    params.sc_params.lookup_count = card_generator_.GetLookupCount();
                    params.total_texels = total_texels;
                    params.card_count = card_count;
                    params.light_count = frame_data.light_count;
                    params._pad = 0;
                    *mapped = params;
                    device_->UnmapBuffer(params_cb_[frameIdx]);
                }
            }

            // ================================================================
            // 3. Single merged LightEval dispatch (1D, per-card texel)
            // ================================================================
            {
                DescriptorData evalParams[] = {
                    {0, DescriptorType::StorageImage,  albedo_atlas_},
                    {1, DescriptorType::StorageImage,  normal_atlas_},
                    {2, DescriptorType::StorageImage,  emissive_atlas_},
                    {3, DescriptorType::StorageImage,  lighting_atlas_[outIdx]},
                    {1, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                    {2, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                    {3, DescriptorType::StorageBuffer, light_info_buffer_},
                    {4, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                };
                UpdateDescriptorSet(device_, light_eval_set_[frameIdx], evalParams, 8);

                cmd->BindComputePipeline(light_eval_pipeline_);
                const DescriptorSetHandle sets[] = { light_eval_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, light_eval_layout_, 0, 1, sets, 0, nullptr);

                u32 groups = (total_texels + 255) / 256;
                cmd->Dispatch(groups, 1, 1);

                std::cerr << "[SurfaceCache] LightEval dispatch: " << total_texels
                          << " texels, " << card_count << " cards, "
                          << groups << " groups" << std::endl;
            }

            // ================================================================
            // 4. Barrier: lighting_atlas_ UAV -> SRV
            // ================================================================
            {
                ResourceBarrier barrier{};
                barrier.resource = lighting_atlas_[outIdx];
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
