#include "VolumePass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include <cmath>

namespace primal::graphics::volume {

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
// ============================================================================

static const std::string SHADER_BASE_DIR = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
static const std::string VOLUME_SHADER_DIR = SHADER_BASE_DIR + "Volume/";

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
    std::string shaderPath = VOLUME_SHADER_DIR + shaderName + ".metal";

    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Volume/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        std::cerr << "[VolumePass] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, VOLUME_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// CPU-side Worley noise generation for 3D texture
// ============================================================================

static float HashFloat3(float x, float y, float z) {
    x = fmodf(x * 443.8975f + 397.2973f, 1e6f); x -= floorf(x);
    y = fmodf(y * 397.2973f + 491.1871f, 1e6f); y -= floorf(y);
    z = fmodf(z * 491.1871f + 443.8975f, 1e6f); z -= floorf(z);
    float d = x * y + y * z + z * x + 19.19f;
    float result = fmodf((x + y) * z * 1234.5678f + d, 1e6f);
    return result - floorf(result);
}

static float WorleyNoise3D(float x, float y, float z) {
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    float minDist = 1e10f;

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                float px = HashFloat3(ix + dx, iy + dy, iz + dz);
                float py = HashFloat3(ix + dx + 37, iy + dy, iz + dz);
                float pz = HashFloat3(ix + dx, iy + dy + 74, iz + dz);
                float distX = dx + px - fx;
                float distY = dy + py - fy;
                float distZ = dz + pz - fz;
                float dist = sqrtf(distX * distX + distY * distY + distZ * distZ);
                if (dist < minDist) minDist = dist;
            }
        }
    }
    return minDist;
}

static float FBMWorley(float x, float y, float z, int octaves) {
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * WorleyNoise3D(x * frequency, y * frequency, z * frequency);
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct VolumePassData {
    RGResourceHandle scatter_output;
};

} // anonymous namespace

// ============================================================================
// VolumePass Implementation
// ============================================================================

VolumePass::~VolumePass() {
    Shutdown();
}

bool VolumePass::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                            const VolumeRuntimeParams& params) {
    if (initialized_) return true;

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    params_ = params;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreateNoiseTexture();
    CreatePersistentBuffers();

    initialized_ = true;

    u32 half_w = render_width / 2;
    u32 half_h = render_height / 2;
    std::cout << "[VolumePass] Initialized (" << render_width << "x" << render_height
              << ", half-res: " << half_w << "x" << half_h << ")" << std::endl;
    return true;
}

void VolumePass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

// ============================================================================
// Private helper methods
// ============================================================================

void VolumePass::CreateDescriptorSetLayouts() {
    // --- Density trace: 5 textures (sampled) + 2 storage buffers + 1 UBO ---
    // Pass 1 reads: depth(texture2D), noise(texture3D), 3x SDF(texture3D)
    // Writes: density_accum, transmittance_log (storage buffers)
    // Metal: texture/buffer binding namespaces are independent
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures (sampled)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_buffer
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // noise_tex
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
            // Buffers (separate Metal namespace)
            {0, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // density_accum
            {1, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // transmittance_log
            {2, DescriptorType::UniformBuffer,  1, ShaderStage::Compute, nullptr},  // VolumeParams
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        density_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Lighting eval: 2 textures (sampled) + 1 storage texture (write) + 2 storage buffers + 1 UBO ---
    // Pass 2 reads: scene_color(texture2D), shadow_map(texture2D), density/transmittance (buffers)
    // Writes: scatter_output (RGBA16_Float storage texture)
    {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures (sampled)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // scene_color
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_map
            // Texture (storage/write)
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output_color
            // Buffers (separate Metal namespace)
            {0, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // density_accum
            {1, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // transmittance_log
            {2, DescriptorType::UniformBuffer,  1, ShaderStage::Compute, nullptr},  // VolumeParams
        };
        DescriptorSetLayoutDesc layoutDesc{6, bindings};
        lighting_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void VolumePass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto densityShader = CompileShader("VolumeTrace", "volume_density_trace");
    auto lightingShader = CompileShader("VolumeTrace", "volume_lighting_eval");

    if (densityShader == handles::INVALID_SHADER ||
        lightingShader == handles::INVALID_SHADER) {
        std::cerr << "[VolumePass] Shader compilation failed: density="
                  << (densityShader == handles::INVALID_SHADER ? "FAIL" : "OK")
                  << " lighting=" << (lightingShader == handles::INVALID_SHADER ? "FAIL" : "OK")
                  << std::endl;
        return;
    }

    // Pipeline layouts
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &density_set_layout_;
        density_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &lighting_set_layout_;
        lighting_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = densityShader;
        pipeDesc.layout = density_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        density_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightingShader;
        pipeDesc.layout = lighting_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        lighting_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{density_set_layout_};
            density_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{lighting_set_layout_};
            lighting_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void VolumePass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 512;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_[i] = device_->CreateBuffer(desc);
    }
}

void VolumePass::CreateNoiseTexture() {
    // 128^3 R32_Float 3D noise texture, CPU-filled with FBM Worley
    constexpr u32 NOISE_SIZE = 128;
    constexpr u32 NOISE_VOXELS = NOISE_SIZE * NOISE_SIZE * NOISE_SIZE;

    // 1. Create texture (Static = Private GPU memory)
    TextureDesc texDesc{};
    texDesc.size = {NOISE_SIZE, NOISE_SIZE, NOISE_SIZE};
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = DataFormat::R32_Float;
    texDesc.type = TextureType::Texture3D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    texDesc.memoryUsage = GPUMemoryUsage::Static;
    texDesc.name = "VolumeNoise3D";
    noise_texture_ = device_->CreateTexture(texDesc);

    if (noise_texture_ == handles::INVALID_RESOURCE) {
        std::cerr << "[VolumePass] Failed to create noise texture" << std::endl;
        return;
    }

    // 2. Generate noise data on CPU
    std::vector<float> noiseData(NOISE_VOXELS);
    for (u32 z = 0; z < NOISE_SIZE; z++) {
        for (u32 y = 0; y < NOISE_SIZE; y++) {
            for (u32 x = 0; x < NOISE_SIZE; x++) {
                float fx = (float)x / NOISE_SIZE * 4.0f;
                float fy = (float)y / NOISE_SIZE * 4.0f;
                float fz = (float)z / NOISE_SIZE * 4.0f;
                noiseData[z * NOISE_SIZE * NOISE_SIZE + y * NOISE_SIZE + x] = FBMWorley(fx, fy, fz, 4);
            }
        }
    }

    // 3. Upload via staging buffer
    // CRITICAL: Use GPUMemoryUsage::Unknown to get Shared (mappable) storage
    // WITHOUT going through RHIAdaptiveMemoryPool. Dynamic/Staging/Readback
    // allocations from the pool corrupt subsequent material buffer allocations.
    BufferDesc stagingDesc{};
    stagingDesc.size = NOISE_VOXELS * sizeof(float);
    stagingDesc.memoryUsage = GPUMemoryUsage::Unknown;
    stagingDesc.name = "VolumeNoiseStaging";
    auto stagingBuffer = device_->CreateBuffer(stagingDesc);

    if (stagingBuffer == handles::INVALID_RESOURCE) {
        std::cerr << "[VolumePass] Failed to create noise staging buffer" << std::endl;
        return;
    }

    float* mapped = static_cast<float*>(device_->MapBuffer(stagingBuffer));
    if (!mapped) {
        std::cerr << "[VolumePass] Failed to map noise staging buffer" << std::endl;
        device_->DestroyBuffer(stagingBuffer);
        return;
    }

    memcpy(mapped, noiseData.data(), NOISE_VOXELS * sizeof(float));
    device_->UnmapBuffer(stagingBuffer);

    // 4. Copy staging buffer → texture via command buffer
    auto cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
        std::cerr << "[VolumePass] Failed to create noise upload command buffer" << std::endl;
        device_->DestroyBuffer(stagingBuffer);
        return;
    }

    auto* cmd = GetCommandBuffer(cmdHandle);
    cmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = NOISE_SIZE;
    region.imageSubresource = {0, 0, 1};
    region.imageExtent = {NOISE_SIZE, NOISE_SIZE, NOISE_SIZE};
    cmd->CopyBufferToTexture(stagingBuffer, noise_texture_, &region, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
    device_->DestroyCommandBuffer(cmdHandle);

    // 5. Destroy staging buffer
    device_->DestroyBuffer(stagingBuffer);

    std::cout << "[VolumePass] Noise texture created and uploaded (128^3, FBM Worley)" << std::endl;
}

void VolumePass::CreatePersistentBuffers() {
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;
    u32 pixelCount = half_w * half_h;

    for (int i = 0; i < 3; i++) {
        {
            // half4 per pixel: RGB = transmittance-weighted scatter, A unused
            BufferDesc desc{};
            desc.size = pixelCount * sizeof(u16) * 4;  // half4 = 8 bytes
            desc.type = BufferType::Structured;
            desc.usage = GPUMemoryUsage::Static;
            desc.memoryUsage = GPUMemoryUsage::Static;
            density_accum_buffer_[i] = device_->CreateBuffer(desc);
        }
        {
            BufferDesc desc{};
            desc.size = pixelCount * sizeof(float);
            desc.type = BufferType::Structured;
            desc.usage = GPUMemoryUsage::Static;   // TEMP: use Static to bypass pool
            desc.memoryUsage = GPUMemoryUsage::Static;
            transmittance_log_buffer_[i] = device_->CreateBuffer(desc);
        }
        // Scatter output texture (triple-buffered, RGBA16_Float, half-res)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::RGBA16_Float;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            scatter_texture_[i] = device_->CreateTexture(desc);
        }
    }
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

VolumeOutput VolumePass::AddPass(RenderGraph& graph, const VolumeInputs& inputs) {
    VolumeOutput output{};

    if (!initialized_) return output;

    u32 outIdx = inputs.camera_data.frame_index % 3;
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    // Import persistent textures into render graph
    auto scatterHandle = graph.ImportResource(
        "VolumeScatter_" + std::to_string(outIdx), scatter_texture_[outIdx]);

    output.volume_scatter = scatterHandle;

    graph.AddPass<VolumePassData>("VolumeRayMarch",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [inputs, scatterHandle](VolumePassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputs.gbuffer_depth, ResourceState::ShaderResource);
            if (inputs.scene_color.IsValid())
                builder.Read(inputs.scene_color,   ResourceState::ShaderResource);
            if (inputs.shadow_map.IsValid())
                builder.Read(inputs.shadow_map,    ResourceState::ShaderResource);
            if (inputs.sdf_cascade_0.IsValid())
                builder.Read(inputs.sdf_cascade_0, ResourceState::ShaderResource);
            if (inputs.sdf_cascade_1.IsValid())
                builder.Read(inputs.sdf_cascade_1, ResourceState::ShaderResource);
            if (inputs.sdf_cascade_2.IsValid())
                builder.Read(inputs.sdf_cascade_2, ResourceState::ShaderResource);

            data.scatter_output = builder.Write(scatterHandle, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: dispatch 2 compute sub-passes
        // ====================================================================
        [this, inputs, outIdx, half_w, half_h](
            const VolumePassData& data, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            constexpr u32 TG = 8;

            // Resolve physical handles from render graph
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle depthTex    = ResolveTexture(inputs.gbuffer_depth);
            ResourceHandle sceneTex    = ResolveTexture(inputs.scene_color);
            ResourceHandle shadowTex   = ResolveTexture(inputs.shadow_map);

            // SDF cascades: use noise_texture_ (valid Texture3D) as fallback
            // to avoid Apple Silicon GPU crash on null texture binding
            ResourceHandle sdfTex0 = ResolveTexture(inputs.sdf_cascade_0);
            if (sdfTex0 == handles::INVALID_RESOURCE) sdfTex0 = noise_texture_;
            ResourceHandle sdfTex1 = ResolveTexture(inputs.sdf_cascade_1);
            if (sdfTex1 == handles::INVALID_RESOURCE) sdfTex1 = noise_texture_;
            ResourceHandle sdfTex2 = ResolveTexture(inputs.sdf_cascade_2);
            if (sdfTex2 == handles::INVALID_RESOURCE) sdfTex2 = noise_texture_;

            // scene_color / shadow_map fallback to depth texture (valid Texture2D)
            if (sceneTex  == handles::INVALID_RESOURCE) sceneTex  = depthTex;
            if (shadowTex == handles::INVALID_RESOURCE) shadowTex = depthTex;

            // ---- Upload VolumeParams constant buffer ----
            {
                auto* mapped = static_cast<VolumeParams*>(device_->MapBuffer(params_cb_[outIdx]));
                if (mapped) {
                    VolumeParams vp{};
                    // Test: small 5x5x5 cube at scene center
                    vp.VolumeOrigin = {-2.5f, 0.0f, -2.5f, 0.0f};
                    vp.VolumeExtent = {5.0f, 5.0f, 5.0f, 0.0f};

                    // Camera
                    vp.CameraPos = {inputs.camera_data.camera_position.x,
                                    inputs.camera_data.camera_position.y,
                                    inputs.camera_data.camera_position.z, 0.0f};

                    // Inverse view-projection
                    math::m4x4 viewProj = inputs.camera_data.view_matrix * inputs.camera_data.proj_matrix;
                    math::m4x4 invViewProj = rhi::math::Inverse(viewProj);
                    vp.InvViewProj[0] = invViewProj.columns[0];
                    vp.InvViewProj[1] = invViewProj.columns[1];
                    vp.InvViewProj[2] = invViewProj.columns[2];
                    vp.InvViewProj[3] = invViewProj.columns[3];

                    // Ray march params
                    vp.StepSize         = params_.step_size;
                    vp.MaxDistance       = params_.max_distance;
                    vp.ExtinctionScale   = params_.extinction_scale;
                    vp.ScatteringAlbedo  = params_.scattering_albedo;
                    vp.DensityThreshold  = params_.density_threshold;
                    vp.DensityFadeRange  = params_.density_fade_range;
                    vp.MaxSteps          = params_.max_steps;
                    vp.FrameIndex        = inputs.camera_data.frame_index;

                    // Screen dims
                    vp.ScreenWidth  = inputs.width;
                    vp.ScreenHeight = inputs.height;
                    vp._pad0[0] = 0.0f;
                    vp._pad0[1] = 0.0f;

                    // SDF cascade data (zeros — populated by caller if available)
                    for (int i = 0; i < 3; i++) {
                        vp.SdfOrigins[i]    = {0, 0, 0, 0};
                        vp.SdfVoxelSizes[i] = {0, 0, 0, 0};
                        vp.SdfExtents[i]    = {0, 0, 0, 0};
                        vp.SdfResolutions[i] = 0;
                    }
                    vp.SdfCascadeCount = 0;
                    vp._pad1[0] = 0.0f;

                    // Light
                    vp.LightDirection = {inputs.camera_data.light_direction.x,
                                         inputs.camera_data.light_direction.y,
                                         inputs.camera_data.light_direction.z, 0.0f};
                    vp.LightColor = {inputs.camera_data.light_color.x,
                                     inputs.camera_data.light_color.y,
                                     inputs.camera_data.light_color.z, 0.0f};

                    *mapped = vp;
                    device_->UnmapBuffer(params_cb_[outIdx]);
                }
            }

            // ================================================================
            // Sub-pass 1: Density Trace (half-res, texture3D only)
            // ================================================================
            if (density_pipeline_ != handles::INVALID_PIPELINE &&
                density_accum_buffer_[outIdx] != handles::INVALID_RESOURCE) {

                DescriptorData densityParams[] = {
                    {0, DescriptorType::SampledImage,  depthTex},
                    {1, DescriptorType::SampledImage,  noise_texture_},
                    {2, DescriptorType::SampledImage,  sdfTex0},
                    {3, DescriptorType::SampledImage,  sdfTex1},
                    {4, DescriptorType::SampledImage,  sdfTex2},
                    // Buffers (separate Metal namespace)
                    {0, DescriptorType::StorageBuffer,  density_accum_buffer_[outIdx]},
                    {1, DescriptorType::StorageBuffer,  transmittance_log_buffer_[outIdx]},
                    {2, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, density_ds_[outIdx], densityParams, 8);

                cmd->BindComputePipeline(density_pipeline_);
                const DescriptorSetHandle sets[] = { density_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, density_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + TG - 1) / TG;
                u32 gy = (half_h + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: density buffers UAV -> SRV
            {
                ResourceBarrier barriers[2]{};
                barriers[0].resource = density_accum_buffer_[outIdx];
                barriers[0].beforeState = ResourceState::UnorderedAccess;
                barriers[0].afterState = ResourceState::ShaderResource;
                barriers[0].subresource = 0xFFFFFFFF;

                barriers[1].resource = transmittance_log_buffer_[outIdx];
                barriers[1].beforeState = ResourceState::UnorderedAccess;
                barriers[1].afterState = ResourceState::ShaderResource;
                barriers[1].subresource = 0xFFFFFFFF;

                cmd->InsertBarrier(barriers, 2);
            }

            // ================================================================
            // Sub-pass 2: Lighting Eval (half-res, buffer + texture2D)
            // ================================================================
            if (lighting_pipeline_ != handles::INVALID_PIPELINE &&
                scatter_texture_[outIdx] != handles::INVALID_RESOURCE) {

                DescriptorData lightingParams[] = {
                    {0, DescriptorType::SampledImage,  sceneTex},
                    {1, DescriptorType::SampledImage,  shadowTex},
                    {2, DescriptorType::StorageImage,   scatter_texture_[outIdx]},
                    // Buffers (separate Metal namespace)
                    {0, DescriptorType::StorageBuffer,  density_accum_buffer_[outIdx]},
                    {1, DescriptorType::StorageBuffer,  transmittance_log_buffer_[outIdx]},
                    {2, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, lighting_ds_[outIdx], lightingParams, 6);

                cmd->BindComputePipeline(lighting_pipeline_);
                const DescriptorSetHandle sets[] = { lighting_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, lighting_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + TG - 1) / TG;
                u32 gy = (half_h + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: scatter UAV -> SRV (for FusionComposite read)
            {
                ResourceBarrier barrier{};
                barrier.resource = scatter_texture_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }
        }
    );

    output.valid = true;
    return output;
}

} // namespace primal::graphics::volume
