#include "FroxelFogPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/Field/FieldDescriptor.h"
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
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

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
                if (std::ifstream(fullPath).good() == false)
                    fullPath = SHADER_BASE_DIR + includeFile;

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
        std::cerr << "[FroxelFogPass] Failed to load shader: " << shaderName << std::endl;
        return {};
    }
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, VOLUME_SHADER_DIR, included);
    return std::vector<u8>(resolved.begin(), resolved.end());
}

// CPU-side Worley noise (same as VolumePass)
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
    for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
    for (int dz = -1; dz <= 1; dz++) {
        float px = HashFloat3(ix+dx, iy+dy, iz+dz);
        float py = HashFloat3(ix+dx+37, iy+dy, iz+dz);
        float pz = HashFloat3(ix+dx, iy+dy+74, iz+dz);
        float dist = sqrtf(powf(dx+px-fx,2)+powf(dy+py-fy,2)+powf(dz+pz-fz,2));
        if (dist < minDist) minDist = dist;
    }
    return minDist;
}

static float FBMWorley(float x, float y, float z, int octaves) {
    float value = 0.0f, amplitude = 0.5f, frequency = 1.0f;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * WorleyNoise3D(x*frequency, y*frequency, z*frequency);
        frequency *= 2.0f; amplitude *= 0.5f;
    }
    return value;
}

struct FroxelPassData {
    RGResourceHandle scatter_output;
};

} // anonymous namespace

// ============================================================================
// FroxelFogPass Implementation
// ============================================================================

FroxelFogPass::~FroxelFogPass() { Shutdown(); }

bool FroxelFogPass::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                                const FroxelGridConfig& config) {
    if (initialized_) return true;
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    config_ = config;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreateNoiseTexture();
    CreateFroxelTextures();
    CreateScatterTextures();

    initialized_ = true;

    std::cout << "[FroxelFogPass] Initialized ("
              << render_width << "x" << render_height
              << ", froxel: " << config_.grid_width << "x"
              << config_.grid_height << "x" << config_.grid_depth << ")"
              << std::endl;
    return true;
}

void FroxelFogPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

void FroxelFogPass::CreateDescriptorSetLayouts() {
    // --- Density inject: 1 storage texture + 5 sampled textures + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // froxel_density (write)
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // noise_tex
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_buffer
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
        };
        DescriptorSetLayoutDesc layoutDesc{7, bindings};
        density_inject_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Light integrate: 1 sampled + 1 storage texture + 1 sampled + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_density
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter (write)
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_map
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        light_integrate_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Resolve: 2 sampled textures + 1 storage texture + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_buffer
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output_color (write)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        resolve_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void FroxelFogPass::CreatePipelines() {
    auto CompileShader = [&](const char* file, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(file);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto densityShader = CompileShader("FroxelDensityInject", "froxel_density_inject");
    auto lightShader   = CompileShader("FroxelLightIntegrate", "froxel_light_integrate");
    auto resolveShader = CompileShader("FroxelResolve", "froxel_resolve");

    if (densityShader == handles::INVALID_SHADER ||
        lightShader   == handles::INVALID_SHADER ||
        resolveShader == handles::INVALID_SHADER) {
        std::cerr << "[FroxelFogPass] Shader compilation failed" << std::endl;
        return;
    }

    // Pipeline layouts
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &density_inject_set_layout_;
        density_inject_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &light_integrate_set_layout_;
        light_integrate_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &resolve_set_layout_;
        resolve_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = densityShader;
        pipeDesc.layout = density_inject_layout_;
        pipeDesc.threadGroupSize = {4, 4, 4};
        density_inject_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightShader;
        pipeDesc.layout = light_integrate_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        light_integrate_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = resolveShader;
        pipeDesc.layout = resolve_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        resolve_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        density_inject_ds_[i] = device_->CreateDescriptorSet({density_inject_set_layout_});
        light_integrate_ds_[i] = device_->CreateDescriptorSet({light_integrate_set_layout_});
        resolve_ds_[i] = device_->CreateDescriptorSet({resolve_set_layout_});
    }
}

void FroxelFogPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 640;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_[i] = device_->CreateBuffer(desc);
    }
}

void FroxelFogPass::CreateNoiseTexture() {
    constexpr u32 NOISE_SIZE = 128;
    constexpr u32 NOISE_VOXELS = NOISE_SIZE * NOISE_SIZE * NOISE_SIZE;

    TextureDesc texDesc{};
    texDesc.size = {NOISE_SIZE, NOISE_SIZE, NOISE_SIZE};
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = DataFormat::R32_Float;
    texDesc.type = TextureType::Texture3D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    texDesc.memoryUsage = GPUMemoryUsage::Static;
    texDesc.name = "FroxelNoise3D";
    noise_texture_ = device_->CreateTexture(texDesc);

    if (noise_texture_ == handles::INVALID_RESOURCE) return;

    std::vector<float> noiseData(NOISE_VOXELS);
    for (u32 z = 0; z < NOISE_SIZE; z++)
    for (u32 y = 0; y < NOISE_SIZE; y++)
    for (u32 x = 0; x < NOISE_SIZE; x++) {
        noiseData[z*NOISE_SIZE*NOISE_SIZE + y*NOISE_SIZE + x] =
            FBMWorley((float)x/NOISE_SIZE*4, (float)y/NOISE_SIZE*4, (float)z/NOISE_SIZE*4, 4);
    }

    BufferDesc stagingDesc{};
    stagingDesc.size = NOISE_VOXELS * sizeof(float);
    stagingDesc.memoryUsage = GPUMemoryUsage::Unknown;
    stagingDesc.name = "FroxelNoiseStaging";
    auto staging = device_->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE) return;

    float* mapped = static_cast<float*>(device_->MapBuffer(staging));
    if (!mapped) { device_->DestroyBuffer(staging); return; }
    memcpy(mapped, noiseData.data(), NOISE_VOXELS * sizeof(float));
    device_->UnmapBuffer(staging);

    auto cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) { device_->DestroyBuffer(staging); return; }

    auto* cmd = GetCommandBuffer(cmdHandle);
    cmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = NOISE_SIZE;
    region.imageSubresource = {0, 0, 1};
    region.imageExtent = {NOISE_SIZE, NOISE_SIZE, NOISE_SIZE};
    cmd->CopyBufferToTexture(staging, noise_texture_, &region, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);

    std::cout << "[FroxelFogPass] Noise texture created (128^3)" << std::endl;
}

void FroxelFogPass::CreateFroxelTextures() {
    for (int i = 0; i < 3; i++) {
        // Density grid (R16_Float)
        {
            TextureDesc desc{};
            desc.size = {config_.grid_width, config_.grid_height, config_.grid_depth};
            desc.format = DataFormat::R16_Float;
            desc.type = TextureType::Texture3D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FroxelDensity";
            froxel_density_[i] = device_->CreateTexture(desc);
        }
        // Scatter grid (RGBA16_Float)
        {
            TextureDesc desc{};
            desc.size = {config_.grid_width, config_.grid_height, config_.grid_depth};
            desc.format = DataFormat::RGBA16_Float;
            desc.type = TextureType::Texture3D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FroxelScatter";
            froxel_scatter_[i] = device_->CreateTexture(desc);
        }
    }
}

void FroxelFogPass::CreateScatterTextures() {
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    for (int i = 0; i < 3; i++) {
        TextureDesc desc{};
        desc.size = {half_w, half_h, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        desc.memoryUsage = GPUMemoryUsage::Static;
        scatter_texture_[i] = device_->CreateTexture(desc);
    }
}

// ============================================================================
// AddPass
// ============================================================================

FroxelOutput FroxelFogPass::AddPass(RenderGraph& graph, const FroxelInputs& inputs) {
    FroxelOutput output{};
    if (!initialized_) return output;

    u32 outIdx = inputs.camera_data.frame_index % 3;
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    auto scatterHandle = graph.ImportResource(
        "FroxelScatter_" + std::to_string(outIdx), scatter_texture_[outIdx]);
    output.volume_scatter = scatterHandle;

    graph.AddPass<FroxelPassData>("FroxelFog",
        RGPassType::Compute, RGPassCategory::Lighting,

        // Setup
        [inputs, scatterHandle](FroxelPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputs.gbuffer_depth, ResourceState::ShaderResource);
            if (inputs.shadow_map.IsValid())
                builder.Read(inputs.shadow_map, ResourceState::ShaderResource);
            if (inputs.sdf_cascade_0.IsValid())
                builder.Read(inputs.sdf_cascade_0, ResourceState::ShaderResource);
            if (inputs.sdf_cascade_1.IsValid())
                builder.Read(inputs.sdf_cascade_1, ResourceState::ShaderResource);
            if (inputs.sdf_cascade_2.IsValid())
                builder.Read(inputs.sdf_cascade_2, ResourceState::ShaderResource);
            data.scatter_output = builder.Write(scatterHandle, ResourceState::UnorderedAccess);
        },

        // Execute: 3 sub-passes
        [this, inputs, outIdx, half_w, half_h](
            const FroxelPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                return res ? res->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            };

            ResourceHandle depthTex  = ResolveTexture(inputs.gbuffer_depth);
            ResourceHandle shadowTex = ResolveTexture(inputs.shadow_map);

            ResourceHandle sdfTex0 = ResolveTexture(inputs.sdf_cascade_0);
            if (sdfTex0 == handles::INVALID_RESOURCE) sdfTex0 = noise_texture_;
            ResourceHandle sdfTex1 = ResolveTexture(inputs.sdf_cascade_1);
            if (sdfTex1 == handles::INVALID_RESOURCE) sdfTex1 = noise_texture_;
            ResourceHandle sdfTex2 = ResolveTexture(inputs.sdf_cascade_2);
            if (sdfTex2 == handles::INVALID_RESOURCE) sdfTex2 = noise_texture_;

            if (shadowTex == handles::INVALID_RESOURCE) shadowTex = depthTex;

            // ---- Upload FroxelParams ----
            {
                auto* mapped = static_cast<FroxelParams*>(device_->MapBuffer(params_cb_[outIdx]));
                if (mapped) {
                    FroxelParams fp{};

                    // Camera
                    fp.CameraPos = {inputs.camera_data.camera_position.x,
                                    inputs.camera_data.camera_position.y,
                                    inputs.camera_data.camera_position.z, 0.0f};

                    math::m4x4 viewProj = inputs.camera_data.view_matrix * inputs.camera_data.proj_matrix;
                    math::m4x4 invViewProj = rhi::math::Inverse(viewProj);
                    fp.InvViewProj[0] = invViewProj.columns[0];
                    fp.InvViewProj[1] = invViewProj.columns[1];
                    fp.InvViewProj[2] = invViewProj.columns[2];
                    fp.InvViewProj[3] = invViewProj.columns[3];

                    // Frustum
                    float near_p = config_.near_plane;
                    float far_p  = config_.far_plane;
                    fp.FrustumParams = {near_p, far_p, log2f(far_p / near_p),
                                        1.0f / (float)config_.grid_depth};

                    // Froxel dims
                    fp.FroxelDims = {(float)config_.grid_width, (float)config_.grid_height,
                                     (float)config_.grid_depth,
                                     1.0f / (float)config_.grid_width};
                    fp.FroxelDims2 = {1.0f / (float)config_.grid_height,
                                      1.0f / (float)config_.grid_depth, 0.0f, 0.0f};

                    // Volume params
                    fp.VolumeParams1 = {config_.extinction_scale, config_.scattering_albedo,
                                        config_.density_threshold, config_.density_fade_range};
                    fp.VolumeParams2 = {config_.height_fog_base, config_.height_fog_scale,
                                        config_.ambient_intensity,
                                        (float)inputs.camera_data.frame_index * 0.016f};
                    fp.VolumeParams3 = {config_.phase_g, config_.noise_scale,
                                        config_.noise_speed, 0.0f};

                    // Screen
                    fp.ScreenParams = {(float)inputs.width, (float)inputs.height,
                                       1.0f / (float)inputs.width,
                                       1.0f / (float)inputs.height};

                    // SDF cascades from FieldRegistry
                    {
                        field::FieldDescriptor sdf_cascades[4];
                        u32 count = field::FieldRegistry::Get().FindCascaded(
                            field::FieldSemantic::GlobalSDF, sdf_cascades, 4);

                        for (u32 c = 0; c < 3; ++c) {
                            if (c < count) {
                                const auto& desc = sdf_cascades[c];
                                fp.SdfOrigins[c]    = {desc.origin.x, desc.origin.y, desc.origin.z, 0.0f};
                                fp.SdfVoxelSizes[c] = {desc.GetFloat(field::FieldAttr::VoxelSize), 0,0,0};
                                fp.SdfExtents[c]    = {desc.extent.x, desc.extent.y, desc.extent.z, 0.0f};
                            } else {
                                fp.SdfOrigins[c] = fp.SdfVoxelSizes[c] = fp.SdfExtents[c] = {0,0,0,0};
                            }
                        }
                        fp.SdfResolutions = {
                            count > 0 ? (float)sdf_cascades[0].GetUInt(field::FieldAttr::Resolution) : 0.0f,
                            count > 1 ? (float)sdf_cascades[1].GetUInt(field::FieldAttr::Resolution) : 0.0f,
                            count > 2 ? (float)sdf_cascades[2].GetUInt(field::FieldAttr::Resolution) : 0.0f,
                            (float)count
                        };
                    }

                    // Light
                    fp.LightDirection = {inputs.camera_data.light_direction.x,
                                         inputs.camera_data.light_direction.y,
                                         inputs.camera_data.light_direction.z, 0.0f};
                    fp.LightColor = {inputs.camera_data.light_color.x,
                                     inputs.camera_data.light_color.y,
                                     inputs.camera_data.light_color.z, 0.0f};

                    // Shadow VP matrices
                    for (u32 c = 0; c < 2; ++c) {
                        const auto& mat = (c == 0) ? inputs.shadow_matrix0 : inputs.shadow_matrix1;
                        fp.ShadowVP[c][0] = mat.columns[0];
                        fp.ShadowVP[c][1] = mat.columns[1];
                        fp.ShadowVP[c][2] = mat.columns[2];
                        fp.ShadowVP[c][3] = mat.columns[3];
                    }

                    *mapped = fp;
                    device_->UnmapBuffer(params_cb_[outIdx]);
                }
            }

            // ================================================================
            // Sub-pass 1: Density Inject
            // ================================================================
            if (density_inject_pipeline_ != handles::INVALID_PIPELINE) {
                DescriptorData params[] = {
                    {0, DescriptorType::StorageImage,  froxel_density_[outIdx]},
                    {1, DescriptorType::SampledImage,  noise_texture_},
                    {2, DescriptorType::SampledImage,  sdfTex0},
                    {3, DescriptorType::SampledImage,  sdfTex1},
                    {4, DescriptorType::SampledImage,  sdfTex2},
                    {5, DescriptorType::SampledImage,  depthTex},
                    {0, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, density_inject_ds_[outIdx], params, 7);

                cmd->BindComputePipeline(density_inject_pipeline_);
                const DescriptorSetHandle sets[] = { density_inject_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, density_inject_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (config_.grid_width  + 3) / 4;
                u32 gy = (config_.grid_height + 3) / 4;
                u32 gz = (config_.grid_depth  + 3) / 4;
                cmd->Dispatch(gx, gy, gz);
            }

            // Barrier: froxel_density UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = froxel_density_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 2: Light Integrate
            // ================================================================
            if (light_integrate_pipeline_ != handles::INVALID_PIPELINE) {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  froxel_density_[outIdx]},
                    {1, DescriptorType::StorageImage,   froxel_scatter_[outIdx]},
                    {2, DescriptorType::SampledImage,  shadowTex},
                    {0, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, light_integrate_ds_[outIdx], params, 4);

                cmd->BindComputePipeline(light_integrate_pipeline_);
                const DescriptorSetHandle sets[] = { light_integrate_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, light_integrate_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (config_.grid_width  + 7) / 8;
                u32 gy = (config_.grid_height + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: froxel_scatter UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = froxel_scatter_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 3: Resolve
            // ================================================================
            if (resolve_pipeline_ != handles::INVALID_PIPELINE) {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  froxel_scatter_[outIdx]},
                    {1, DescriptorType::SampledImage,  depthTex},
                    {2, DescriptorType::StorageImage,   scatter_texture_[outIdx]},
                    {0, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, resolve_ds_[outIdx], params, 4);

                cmd->BindComputePipeline(resolve_pipeline_);
                const DescriptorSetHandle sets[] = { resolve_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, resolve_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + 7) / 8;
                u32 gy = (half_h + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: scatter_texture UAV -> SRV (for FusionComposite)
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
