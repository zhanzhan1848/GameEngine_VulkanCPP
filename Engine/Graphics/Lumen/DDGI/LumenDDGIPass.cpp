#include "LumenDDGIPass.h"
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Dawn/ShaderLoader.h"  // for dawn::LoadWGSL
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdio>
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

static std::vector<u8> LoadShaderBytecode(const char* shaderName, rhi::RHIDeviceBase* device) {
    auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;

    if (platform == rhi::RHIPlatform::Dawn) {
        // WGSL path. dawn::LoadWGSL has two overloads (ShaderLoader.h):
        //   WASM:   LoadWGSL(const char* name) — kShaderMap + MEMFS fallback at
        //           /Engine/Graphics/Dawn/shaders/{name}.wgsl
        //   Native: LoadWGSL(const std::string& path) — opens filesystem path as-is
        // DDGI shaders live in Engine/Graphics/Dawn/shaders/Lumen/. On native
        // we pass the full path. On WASM, Emscripten --embed-file preserves
        // subdirs, so the shader is at /Engine/Graphics/Dawn/shaders/Lumen/ in
        // MEMFS — pass a relative name and let LoadWGSL's fallback construct
        // the right path.
        std::string src;
#ifdef __EMSCRIPTEN__
        std::string lumenName = std::string("Lumen/") + shaderName;
        src = dawn::LoadWGSL(lumenName.c_str());
#else
        std::string path = std::string("Engine/Graphics/Dawn/shaders/Lumen/") + shaderName + ".wgsl";
        src = dawn::LoadWGSL(path);
#endif
        if (src.empty()) {
            std::cerr << "[LumenDDGI] Failed to load WGSL shader: " << shaderName << std::endl;
            return {};
        }
        return std::vector<u8>(src.begin(), src.end());
    }

    // Metal path (unchanged): file read + #include resolution + /tmp/ dump
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
        updateListDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        for (u32 i = 0; i < 3; ++i) {
            probe_update_list_buffer_[i] = device_->CreateBuffer(updateListDesc);
        }
    }

    // Initialize probes from static bake data (or sky estimate fallback)
    InitializeProbesFromStatic();

    initialized_ = true;

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
    // --- Trace: 4 sampled textures + 2 UBO + 3 SSBO ---
    // Platform-branch: Dawn uses sequential 0..8 (no texture/buffer collision).
    // Metal overlaps texture/buffer namespaces — DDGITraceRays.metal declares [[buffer(0..4)]].
    {
        bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
        if (isDawn) {
            // Dawn: sequential engine bindings 0..9 matching WGSL
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
            // Metal: separate texture/buffer namespaces -- overlap is idiomatic.
            // DDGITraceRays.metal declares [[buffer(0..4)]] for the 5 buffers.
            DescriptorSetLayoutBinding traceBindings[] = {
                // Textures
                {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
                // Buffers (Metal namespace -- overlap with textures is fine)
                {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
                {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
                {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            };
            traceBindings[0].is3D = true;
            traceBindings[1].is3D = true;
            traceBindings[2].is3D = true;
            traceBindings[7].readonly = true;
            traceBindings[8].readonly = true;
            DescriptorSetLayoutDesc layoutDesc{9, traceBindings};
            trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        }
    }

    // --- Irradiance: 2 UBO + 3 SSBO (ray data + irradiance history + irradiance output) + 1 SSBO (update list) ---
    {
        DescriptorSetLayoutBinding irradianceBindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance history buffer (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance output buffer (read_write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list (read)
        };
        irradianceBindings[2].readonly = true;  // ray_buffer: var<storage, read>
        irradianceBindings[3].readonly = true;  // irradiance_history: var<storage, read>
        irradianceBindings[5].readonly = true;  // probe_update_list: var<storage, read>
        // irradianceBindings[4] (irradiance_output) stays read_write.
        DescriptorSetLayoutDesc layoutDesc{6, irradianceBindings};
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
        };
        depthBindings[2].readonly = true;  // ray_buffer: var<storage, read>
        depthBindings[3].readonly = true;  // depth_history: var<storage, read>
        depthBindings[5].readonly = true;  // probe_update_list: var<storage, read>
        // depthBindings[4] (depth_output) stays read_write.
        DescriptorSetLayoutDesc layoutDesc{6, depthBindings};
        depth_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void LumenDDGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name, device_);
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

    // Depth buffers: octahedral 8x8 depth map per probe
    // Layout per probe: [mean0..mean63, variance0..variance63] = 128 floats
    // Apple Silicon: buffer load/store is much faster than texture3D sampling
    static constexpr u32 DDGI_DEPTH_RES = 8;
    static constexpr u32 DDGI_DEPTH_TEXELS = DDGI_DEPTH_RES * DDGI_DEPTH_RES; // 64
    static constexpr u32 DDGI_FLOATS_PER_PROBE = DDGI_DEPTH_TEXELS * 2; // 128
    u64 depthSize = (u64)totalProbes * DDGI_FLOATS_PER_PROBE * sizeof(float);
    float depthMeanInit = params_.ray_max_distance;  // start "open" — no occlusion assumed
    float depthVarInit  = params_.probe_spacing * params_.probe_spacing * 0.25f; // non-zero avoids Chebyshev singularity

    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = depthSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
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
            if (static_volume_ && static_volume_->IsLoaded() && !dynamic_mode_) {
                return;
            }

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
                    std::cerr << "[Mode11] GlobalSDF unavailable — falling back to static seed\n";
                    warned = true;
                }
                return;
            }
            if (dynamic_mode_) {
                static bool traced = false;
                if (!traced) {
                    std::cerr << "[Mode11] GlobalSDF available — runtime trace active\n";
                    traced = true;
                }
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
                    // Mode 11 canonical: .w = light intensity (matches
                    // ProbeBakingScene::light_intensity = 3.0 from
                    // TestDawnForwardRenderer.cpp:2407).
                    vd.LightColor = {camera_data.light_color.x,
                                     camera_data.light_color.y,
                                     camera_data.light_color.z, 3.0f};
                    // Mode 11 canonical radiance inputs — match bake's
                    // ProbeBakingScene at TestDawnForwardRenderer.cpp:2409 and
                    // StaticProbeBaker.h:16 default. Drives E_direct, E_sky, and
                    // (albedo/PI) in the canonical L_out formula at SDF hit.
                    vd.SkyColor = {0.6f, 0.6f, 0.7f, 0.0f};
                    vd.Albedo   = {0.6f, 0.6f, 0.6f, 0.0f};

                    // Fill SDF cascade data from GlobalSDF
                    for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                        const auto& cascade = sdf.GetCascade(c);
                        vd.SdfOrigins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                        vd.SdfVoxelSizes[c] = {cascade.voxel_size, 0.0f, 0.0f, 0.0f};
                        vd.SdfExtents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                        vd.SdfResolutions[c] = cascade.resolution;
                    }
                    vd.SdfCascadeCount = sdf.GetConfig().cascade_count;

                    vd.ProbeUpdateCount = probeCountTotal;  // default: update all; refined below
                    vd._pad_before_relocation = 0.0f;

                    *mapped = vd;
                    device_->UnmapBuffer(volume_cb_[frameIdx]);

                    // Store for C++ access via GetVolumeData()
                    volume_data_ = vd;
                }
            }

            // ================================================================
            // Priority scheduling: select top-N probes to update this frame
            // ================================================================
            u32 updateCount = probeCountTotal;  // default: update all probes

            // First frame: full update of all probes to initialize irradiance/depth.
            // Subsequent frames: priority scheduling updates only top-N probes.
            bool isFirstFrame = (camera_data.frame_index <= 1);

            // Read back variance from previous frame's depth buffer for priority scheduling.
            // histIdx is 2 frames behind — GPU has long since finished writing to it.
            if (!isFirstFrame &&
                probe_states_.size() == probeCountTotal &&
                probe_update_list_buffer_[frameIdx] != handles::INVALID_RESOURCE) {
                float* depthData = static_cast<float*>(device_->MapBuffer(depth_buffers_[histIdx]));
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
                    device_->UnmapBuffer(depth_buffers_[histIdx]);
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
                    cameraPos = { invView.columns[3][0], invView.columns[3][1], invView.columns[3][2] };
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
                    float ageScore = std::min(float(current_frame_index - probe_states_[i].last_update_frame), 30.0f) / 30.0f;

                    priorities[i] = { i, distScore + varianceScore * 0.5f + ageScore * 0.3f };
                }

                // Partial sort: top-N only
                updateCount = std::min(max_probes_per_frame_, probeCountTotal);
                std::partial_sort(priorities.begin(), priorities.begin() + updateCount,
                                  priorities.end(), [](const ProbePriority& a, const ProbePriority& b) {
                                      return a.score > b.score;
                                  });

                // Write update list buffer
                u32* updateList = static_cast<u32*>(device_->MapBuffer(probe_update_list_buffer_[frameIdx]));
                if (updateList) {
                    for (u32 i = 0; i < updateCount; ++i) {
                        updateList[i] = priorities[i].index;
                        probe_states_[priorities[i].index].last_update_frame = current_frame_index;
                    }
                    device_->UnmapBuffer(probe_update_list_buffer_[frameIdx]);
                }

                // Patch ProbeUpdateCount in the volume constant buffer
                {
                    auto* mapped = static_cast<DDGIVolumeData*>(device_->MapBuffer(volume_cb_[frameIdx]));
                    if (mapped) {
                        mapped->ProbeUpdateCount = updateCount;
                        device_->UnmapBuffer(volume_cb_[frameIdx]);
                        volume_data_.ProbeUpdateCount = updateCount;
                    }
                }
            }

            // First frame: fill update list with all probe indices (0..N-1)
            if (isFirstFrame && probe_update_list_buffer_[frameIdx] != handles::INVALID_RESOURCE) {
                u32* updateList = static_cast<u32*>(device_->MapBuffer(probe_update_list_buffer_[frameIdx]));
                if (updateList) {
                    for (u32 i = 0; i < updateCount; ++i) {
                        updateList[i] = i;
                    }
                    device_->UnmapBuffer(probe_update_list_buffer_[frameIdx]);
                }
            }

            // ================================================================
            // Sub-pass 1: TraceRays (skip if GlobalSDF not available)
            // ================================================================
            if (sdfAvailable &&
                trace_pipeline_ != handles::INVALID_PIPELINE &&
                ray_data_buffer_ != handles::INVALID_RESOURCE) {
                // Update trace descriptor set
                // Platform-branch: Dawn uses sequential 0..9 (added GBuffer albedo
                // at binding 9 for Mode 11); Metal keeps 0..8 (Metal shader wasn't
                // updated — Metal runs Mode 10 only).
                bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
                DescriptorData traceParams[10];
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
                    traceParams[7] = {7, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]};
                    traceParams[8] = {8, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]};
                    traceParams[9] = {9, DescriptorType::SampledImage,  gbufferAlbedoTex};
                    traceParamCount = 10;
                } else {
                    // Metal: overlap texture/buffer namespaces -- buffers at 0..4
                    traceParams[0] = {0, DescriptorType::SampledImage,  sdfTextures[0]};
                    traceParams[1] = {1, DescriptorType::SampledImage,  sdfTextures[1]};
                    traceParams[2] = {2, DescriptorType::SampledImage,  sdfTextures[2]};
                    traceParams[3] = {3, DescriptorType::SampledImage,  prevColorTex};
                    traceParams[4] = {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]};
                    traceParams[5] = {1, DescriptorType::UniformBuffer, volume_cb_[frameIdx]};
                    traceParams[6] = {2, DescriptorType::StorageBuffer, ray_data_buffer_};
                    traceParams[7] = {3, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]};
                    traceParams[8] = {4, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]};
                    traceParamCount = 9;
                }
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
                    {5, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]},
                };
                UpdateDescriptorSet(device_, irradiance_ds_[frameIdx], irradianceParams, 6);

                cmd->BindComputePipeline(irradiance_pipeline_);
                const DescriptorSetHandle sets[] = { irradiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, irradiance_layout_, 0, 1, sets, 0, nullptr);

                cmd->Dispatch((updateCount + 63) / 64, 1, 1);
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
                    {5, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]},
                };
                UpdateDescriptorSet(device_, depth_ds_[frameIdx], depthParams, 6);

                cmd->BindComputePipeline(depth_pipeline_);
                const DescriptorSetHandle sets[] = { depth_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, depth_layout_, 0, 1, sets, 0, nullptr);

                cmd->Dispatch((updateCount + 63) / 64, 1, 1);
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

// ============================================================================
// InitializeProbesFromStatic
// ============================================================================
// Seeds the dynamic DDGI probe buffers from a CPU static bake (if a
// StaticProbeVolume is attached and dimensions match), or falls back to a
// sky estimate. Called once at the end of Initialize().
void LumenDDGIPass::InitializeProbesFromStatic() {
    u32 probeCount = params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;

    if (static_volume_ && static_volume_->IsLoaded()) {
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
        std::cout << "[LumenDDGI] Synced probe_origin_ to static bake: ("
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

        std::cout << "[LumenDDGI] Initialized probes from static bake data ("
                  << probeCount << " probes)" << std::endl;
        return;
    }

sky_estimate:
    // No bake data or dimension mismatch: sky estimate fallback
    // Must use float* (not math::v3*) — buffer is 12 bytes/element, math::v3 is 16 bytes.
    math::v3 skyL0 = math::v3{0.5f, 0.7f, 1.0f} * 3.14159265f; // sky color * pi
    for (int f = 0; f < 3; f++) {
        u64 irrSize = (u64)probeCount * 9 * 3 * sizeof(float);
        float* mapped = static_cast<float*>(device_->MapBuffer(irradiance_buffers_[f]));
        if (mapped) {
            memset(mapped, 0, irrSize);
            for (u32 p = 0; p < probeCount; p++) {
                mapped[p * 27 + 0] = skyL0.x;
                mapped[p * 27 + 1] = skyL0.y;
                mapped[p * 27 + 2] = skyL0.z;
            }
            device_->UnmapBuffer(irradiance_buffers_[f]);
        }
    }

    std::cout << "[LumenDDGI] Initialized probes with sky estimate fallback" << std::endl;
}

} // namespace primal::graphics::lumen
