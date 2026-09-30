#include "FroxelFogPass.h"
#include "Graphics/Utils/ShaderRegistry.h"
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
#include <filesystem>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include <cmath>
#include "Engine/Common/CoreLogStream.h"

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

static const std::string SHADER_BASE_DIR =
    utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Metal/shaders/") + "/";
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

static std::vector<u8> LoadShaderBytecode(const char* shaderName, rhi::RHIPlatform platform) {
    // P2.1 (2026-09-15): Vulkan 走预编译 SPIR-V（build_spv.sh 的 Volume/
    // 条目）；MSL 文本喂给 Vulkan shader 模块是无效 pCode（08738，
    // "FroxelFogPass Shader compilation failed" 的根因）。
    if (platform == rhi::RHIPlatform::Vulkan) {
        const char* spvPaths[] = {
            "Engine/Graphics/Vulkan/shaders/Volume/",
            "Darwin/Debug/Engine/Graphics/Vulkan/shaders/Volume/",
        };
        for (const char* dir : spvPaths) {
            std::ifstream f(std::string(dir) + shaderName + ".comp.spv",
                            std::ios::binary | std::ios::ate);
            if (!f.is_open()) continue;
            std::streamsize sz = f.tellg();
            f.seekg(0);
            std::vector<u8> code(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(code.data()), sz);
            // 2026-09-30: 一次性加载源诊断（cwd 相对路径歧义排查）
            static std::set<std::string> logged;
            if (logged.insert(shaderName).second) {
                PRIMAL_LOG_STREAM(Info, "Engine")
                    << "[FroxelFogPass] loaded spv " << shaderName
                    << " from ./" << dir << " (" << sz << " bytes, cwd="
                    << std::filesystem::current_path().string() << ")" << std::endl;
            }
            return code;
        }
        PRIMAL_LOG_STREAM(Error, "Engine") << "[FroxelFogPass] Failed to load spv: " << shaderName << std::endl;
        return {};
    }
    std::string shaderPath = VOLUME_SHADER_DIR + shaderName + ".metal";
    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Volume/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }
    if (source.empty()) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[FroxelFogPass] Failed to load shader: " << shaderName << std::endl;
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

static float smoothstepf(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
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

    // P2-B: 1x1 2D 占位纹理——shadow moments 空槽位的 stand-in（必须是 2D
    // 以匹配 shader 的 texture2D 绑定；噪声纹理是 3D，仅适用于 SDF 槽位）。
    {
        TextureDesc dummyDesc{};
        dummyDesc.size = {1, 1, 1};
        dummyDesc.mipLevels = 1;
        dummyDesc.arraySize = 1;
        dummyDesc.format = DataFormat::R32_Float;
        dummyDesc.type = TextureType::Texture2D;
        dummyDesc.usage = TextureUsage::ShaderResource;
        dummyDesc.memoryUsage = GPUMemoryUsage::Static;
        dummyDesc.name = "FroxelDummyShadow2D";
        dummy_shadow_2d_ = device_->CreateTexture(dummyDesc);
    }
    // WP2: 1x1 黑 2D——spgi_ambient 空槽位（RGB 采样需 RGBA 格式）。
    {
        TextureDesc dummyDesc{};
        dummyDesc.size = {1, 1, 1};
        dummyDesc.mipLevels = 1;
        dummyDesc.arraySize = 1;
        dummyDesc.format = DataFormat::RGBA16_Float;
        dummyDesc.type = TextureType::Texture2D;
        dummyDesc.usage = TextureUsage::ShaderResource;
        dummyDesc.memoryUsage = GPUMemoryUsage::Static;
        dummyDesc.name = "FroxelDummyAmbient2D";
        dummy_ambient_2d_ = device_->CreateTexture(dummyDesc);
    }
    if (config_.cloud_enable > 0.0f)
        CreateCloudTexture();
    CreateFroxelTextures();
    CreateScatterTextures();

    initialized_ = true;

    PRIMAL_LOG_STREAM(Info, "Engine") << "[FroxelFogPass] Initialized ("
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
    // P2.1 (2026-09-15): Vulkan 扁平绑定命名空间下 UBO 与 StorageImage 不
    // 可共占 binding 0（00279 duplicate binding）——UBO 重编号为 6/3/3
    // （原 Metal 双命名空间写法；MSL 的 [[buffer(N)]] 已同步改号）。
    // --- Density inject: 1 storage texture + 5 sampled textures + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // froxel_density (write)
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // noise_tex
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_buffer
            {6, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // WP5 cloud volume
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        density_inject_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Light integrate: 1 sampled + 1 storage texture + 4 shadow moments
    //     + WP2 SPGI ambient + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_density
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter (write)
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // legacy shadow_map (unused at mode!=0)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
            // P2-B: light-space VSM moments per cascade
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_moments cascade 0
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_moments cascade 1
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_moments cascade 2
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadow_moments cascade 3
            // WP2: 上一帧 SPGI 辐照（ambient_source=1 时采样；无效时绑 1x1 黑）
            {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // spgi_ambient
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        light_integrate_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- WP3 Reproject: 2 sampled 3D + 1 storage 3D + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter_curr
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter_prev
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter_out (write)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        reproject_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Resolve: 2 sampled textures + 1 storage texture + 1 UBO ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // froxel_scatter
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_buffer
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output_color (write)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FroxelParams
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        resolve_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void FroxelFogPass::CreatePipelines() {
    auto CompileShader = [&](const char* file, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(file, device_->GetPlatform());
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto densityShader = CompileShader("FroxelDensityInject", "froxel_density_inject");
    auto lightShader   = CompileShader("FroxelLightIntegrate", "froxel_light_integrate");
    auto resolveShader = CompileShader("FroxelResolve", "froxel_resolve");
    // WP3: reproject 失败不阻塞其余三 pass（temporal_enabled_ 由编译结果门控）
    auto reprojectShader = CompileShader("FroxelReproject", "froxel_reproject");

    if (densityShader == handles::INVALID_SHADER ||
        lightShader   == handles::INVALID_SHADER ||
        resolveShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[FroxelFogPass] Shader compilation failed" << std::endl;
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
    if (reprojectShader != handles::INVALID_SHADER) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &reproject_set_layout_;
        reproject_layout_ = device_->CreatePipelineLayout(plDesc);
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
    if (reprojectShader != handles::INVALID_SHADER) {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = reprojectShader;
        pipeDesc.layout = reproject_layout_;
        pipeDesc.threadGroupSize = {4, 4, 4};
        reproject_pipeline_ = device_->CreateComputePipeline(pipeDesc);
        temporal_available_ = true;
        PRIMAL_LOG_STREAM(Info, "Engine") << "[FroxelFogPass] Reproject pipeline ready (WP3 temporal)" << std::endl;
    }

    // Triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        density_inject_ds_[i] = device_->CreateDescriptorSet({density_inject_set_layout_});
        light_integrate_ds_[i] = device_->CreateDescriptorSet({light_integrate_set_layout_});
        resolve_ds_[i] = device_->CreateDescriptorSet({resolve_set_layout_});
        if (reproject_pipeline_ != handles::INVALID_PIPELINE)
            reproject_ds_[i] = device_->CreateDescriptorSet({reproject_set_layout_});
    }
}

void FroxelFogPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = sizeof(FroxelParams); // 656（FrustumParams2 扩展后）
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
    // P2.1: Unknown → device-local，MapBuffer 触发 00682（映射非
    // HOST_VISIBLE）。Staging 语义即 CPU 写入 + 拷贝源。
    stagingDesc.memoryUsage = GPUMemoryUsage::Staging;
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
    // P0（sponza-gap-reaudit §4.4）: 上传后把布局收敛到 ShaderResource。
    // CopyBufferToTexture 停在 TRANSFER_DST_OPTIMAL 且此纹理不经 RenderGraph
    // 管理，首次 density inject 采样即报 TRANSFER_DST vs SHADER_READ_ONLY
    // validation（本轮 20 条）。与 CopyTextureToBuffer 的 P0-B 收尾惯例一致。
    ResourceBarrier noiseBarrier{};
    noiseBarrier.resource = noise_texture_;
    noiseBarrier.beforeState = ResourceState::CopyDest;
    noiseBarrier.afterState = ResourceState::ShaderResource;
    noiseBarrier.subresource = 0xFFFFFFFF;
    cmd->InsertBarrier(&noiseBarrier, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);

    PRIMAL_LOG_STREAM(Info, "Engine") << "[FroxelFogPass] Noise texture created (128^3)" << std::endl;
}

void FroxelFogPass::CreateCloudTexture() {
    // WP5（3D 纹理路线，用户建议对齐参考实现）：CPU 一次性烘焙积云体积。
    // 形状 = 球形包络（y 压扁 + 云底淡出）× 三尺度细节（大团块 plateau
    // + FBM 中尺度 + Worley 蚀边）。CPU 生成可验证、确定性强，规避
    // inject 内程序化噪声的不可见问题（v7/v8/cumulus_v1 排查记录）。
    constexpr u32 RES = 96;
    TextureDesc desc{};
    desc.size = {RES, RES, RES};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    // R32_Float：与 128³ 噪声纹理完全同格式同上传路径（已验证）；R8 在该
    // 路径上采样恒 0（2026-09-30 排查），疑 RHI 对 1 字节格式的拷贝处理。
    desc.format = DataFormat::R32_Float;
    desc.type = TextureType::Texture3D;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "FroxeCloudVolume";
    cloud_texture_ = device_->CreateTexture(desc);
    if (cloud_texture_ == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[FroxelFogPass] cloud volume create failed" << std::endl;
        return;
    }

    std::vector<float> voxels(size_t(RES) * RES * RES);
    const float freq = std::max(config_.cloud_noise_freq, 0.05f);
    for (u32 z = 0; z < RES; ++z)
    for (u32 y = 0; y < RES; ++y)
    for (u32 x = 0; x < RES; ++x) {
        float px = (float(x) + 0.5f) / RES * 2.0f - 1.0f;
        float py = (float(y) + 0.5f) / RES * 2.0f - 1.0f;
        float pz = (float(z) + 0.5f) / RES * 2.0f - 1.0f;
        float qy = py * 1.05f;
        float r = std::sqrt(px * px + qy * qy + pz * pz);
        if (r >= 1.0f) { voxels[z*RES*RES + y*RES + x] = 0.0f; continue; }
        float body = 1.0f - smoothstepf(0.40f, 1.0f, r);
        body *= 0.55f + 0.45f * smoothstepf(-0.50f, -0.12f, py);

        const float wx = px * config_.cloud_radius;
        const float wy = py * config_.cloud_radius;
        const float wz = pz * config_.cloud_radius;
        float big = WorleyNoise3D(wx * freq * 0.85f + 31.7f,
                                  wy * freq * 0.85f + 11.3f,
                                  wz * freq * 0.85f + 7.9f);
        float lumps = smoothstepf(0.30f, 0.62f, big);
        float mid = FBMWorley(wx * freq * 1.7f + 5.1f,
                              wy * freq * 1.7f + 13.7f,
                              wz * freq * 1.7f + 3.3f, 3);
        float det = WorleyNoise3D(wx * freq * 3.4f + 91.3f,
                                  wy * freq * 3.4f + 47.9f,
                                  wz * freq * 3.4f + 23.1f);
        float shell = 1.0f - 0.55f * smoothstepf(0.45f, 0.95f, det);
        float d = body * clampf(lumps * (0.35f + 0.65f * mid) * shell * 1.9f,
                                0.06f, 1.0f);
        voxels[z*RES*RES + y*RES + x] = clampf(d, 0.0f, 1.0f);   // 归一化 float 密度
    }

    {
        u64 nz = 0; double sum = 0.0; float mx = 0.0f;
        for (float v : voxels) { if (v > 0.03f) ++nz; sum += v; mx = std::max(mx, v); }
        PRIMAL_LOG_STREAM(Info, "Engine") << "[FroxelFogPass] cloud voxels nonzero="
                  << nz << "/" << voxels.size() << " mean=" << sum / voxels.size()
                  << " max=" << mx << std::endl;
    }
    BufferDesc sdesc{};
    sdesc.size = voxels.size() * sizeof(float);
    sdesc.memoryUsage = GPUMemoryUsage::Staging;
    sdesc.name = "FroxeCloudStaging";
    auto staging = device_->CreateBuffer(sdesc);
    if (staging == handles::INVALID_RESOURCE) return;
    if (float* mapped = static_cast<float*>(device_->MapBuffer(staging))) {
        memcpy(mapped, voxels.data(), voxels.size() * sizeof(float));
        device_->UnmapBuffer(staging);

        auto cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
            auto* cmd = GetCommandBuffer(cmdHandle);
            cmd->Begin();
            BufferTextureCopyRegion region{};
            region.bufferOffset = 0;
            region.bufferRowLength = RES;
            region.imageSubresource = {0, 0, 1};
            region.imageExtent = {RES, RES, RES};
            cmd->CopyBufferToTexture(staging, cloud_texture_, &region, 1);
            ResourceBarrier barrier{};
            barrier.resource = cloud_texture_;
            barrier.beforeState = ResourceState::CopyDest;
            barrier.afterState = ResourceState::ShaderResource;
            barrier.subresource = 0xFFFFFFFF;
            cmd->InsertBarrier(&barrier, 1);
            cmd->End();
            cmd->Submit();
            cmd->WaitForCompletion();
            device_->DestroyCommandBuffer(cmdHandle);
            PRIMAL_LOG_STREAM(Info, "Engine") << "[FroxelFogPass] cloud volume baked ("
                      << RES << "^3 R8)" << std::endl;
        }
    }
    device_->DestroyBuffer(staging);
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
        // WP3: temporal 混合结果（resolve 采样源 + 下一帧 reproject 的 prev）
        {
            TextureDesc desc{};
            desc.size = {config_.grid_width, config_.grid_height, config_.grid_depth};
            desc.format = DataFormat::RGBA16_Float;
            desc.type = TextureType::Texture3D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FroxelScatterHist";
            froxel_scatter_hist_[i] = device_->CreateTexture(desc);
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

    // P0（reaudit §4.4）: 噪声纹理纳入图依赖——density inject 每帧采样它
    // （且在 SDF 级联缺失时顶替 3D 槽位），此前绕过 RG 无首次使用审计。
    rendergraph::RGResourceHandle noiseRG{};
    if (noise_texture_ != handles::INVALID_RESOURCE)
        noiseRG = graph.ImportResource("FroxelNoise3D", noise_texture_);

    graph.AddPass<FroxelPassData>("FroxelFog",
        RGPassType::Compute, RGPassCategory::Lighting,

        // Setup
        [inputs, scatterHandle, noiseRG](FroxelPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputs.gbuffer_depth, ResourceState::ShaderResource);
            if (noiseRG.IsValid())
                builder.Read(noiseRG, ResourceState::ShaderResource);
            // P2-B: light-space moments are the shadow input of record. The
            // legacy screen-space shadow_map is no longer read by the
            // integrate shader (coordinate contract fixed).
            for (u32 c = 0; c < 4; ++c) {
                if (inputs.shadow_moments[c].IsValid())
                    builder.Read(inputs.shadow_moments[c], ResourceState::ShaderResource);
                // P1（reaudit §4.4）: PCSS light-space 深度图（mode 2）
                if (inputs.shadow_depth[c].IsValid())
                    builder.Read(inputs.shadow_depth[c], ResourceState::ShaderResource);
            }
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

            // P2-B: resolve light-space moments (mode selector for the shader).
            // 首帧把 2D 占位纹理 prime 到 SRO（创建时是 UNDEFINED，本帧描述符
            // 即承诺 SRO；后端按 tracked 布局发射转换）。
            {
                static bool s_dummyPrimed = false;
                if (!s_dummyPrimed && dummy_shadow_2d_ != handles::INVALID_RESOURCE) {
                    ResourceBarrier primeBarrier{};
                    primeBarrier.resource = dummy_shadow_2d_;
                    primeBarrier.beforeState = ResourceState::ShaderResource;
                    primeBarrier.afterState = ResourceState::ShaderResource;
                    primeBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&primeBarrier, 1);
                    s_dummyPrimed = true;
                }
            }
            ResourceHandle momentTex[4] = {dummy_shadow_2d_, dummy_shadow_2d_,
                                           dummy_shadow_2d_, dummy_shadow_2d_};
            u32 shadowMode = 0;
            for (u32 c = 0; c < 4 && c < inputs.shadow_cascade_count; ++c) {
                ResourceHandle m = ResolveTexture(inputs.shadow_moments[c]);
                if (m != handles::INVALID_RESOURCE) {
                    momentTex[c] = m;
                    shadowMode = 1;
                }
            }
            // P1（reaudit §4.4）: 默认 PCSS 场景不产 VSM moments——回退消费
            // 与 PCSS 共用的 light-space 深度图（D32→R32 blit），shadow_mode=2
            // 让 integrate 逐 froxel 做深度比较。2026-09-30 Metal MSL 已移植
            // 该分支（FroxelLightIntegrate.metal 双模式），撤 Vulkan-only 门。
            if (shadowMode == 0) {
                for (u32 c = 0; c < 4 && c < inputs.shadow_cascade_count; ++c) {
                    ResourceHandle d = ResolveTexture(inputs.shadow_depth[c]);
                    if (d != handles::INVALID_RESOURCE) {
                        momentTex[c] = d;
                        shadowMode = 2;
                    }
                }
            }

            ResourceHandle sdfTex0 = ResolveTexture(inputs.sdf_cascade_0);
            if (sdfTex0 == handles::INVALID_RESOURCE) sdfTex0 = noise_texture_;
            ResourceHandle sdfTex1 = ResolveTexture(inputs.sdf_cascade_1);
            if (sdfTex1 == handles::INVALID_RESOURCE) sdfTex1 = noise_texture_;
            ResourceHandle sdfTex2 = ResolveTexture(inputs.sdf_cascade_2);
            if (sdfTex2 == handles::INVALID_RESOURCE) sdfTex2 = noise_texture_;

            // WP2: SPGI 辐照纹理。无效句柄 → 1x1 黑占位；CB 里的
            // ambient_source 在下方 Upload 段按同一有效性判定强制 0
            //（shader 不采样，占位绑定只为描述符集完整）。
            ResourceHandle ambientTex = inputs.spgi_ambient_tex;
            if (ambientTex == handles::INVALID_RESOURCE)
                ambientTex = dummy_ambient_2d_;
            // 已知启动期瞬态（2026-09-30 记录）：SPGI 重建窗口（GI
            // content-ready 同帧）内，output_texture_filtered_ 的句柄值被
            // 复用而其 view 短暂为 0x0，binding 8 会报 ~18 条 validation
            // 警告后自愈（句柄值不变故无法在 froxel 侧探测；任何消费该
            // 纹理的裸绑定都会暴露）。不崩溃、结果正确，留待 SPGI 侧
            // 重建时序治理。

            // P2-B: 旧的 depthTex fallback 已删除——camera depth 数值不是
            // visibility。无任何阴影输入时 shadowMode=0，shader 恒 lit，
            // 槽位绑 1x1 占位纹理；mode 1=VSM moments，mode 2=PCSS 深度。
            (void)shadowTex;

            // ---- Upload FroxelParams ----
            {
                auto* mapped = static_cast<FroxelParams*>(device_->MapBuffer(params_cb_[outIdx]));
                if (mapped) {
                    FroxelParams fp{};

                    // Camera
                    fp.CameraPos = {inputs.camera_data.camera_position.x,
                                    inputs.camera_data.camera_position.y,
                                    inputs.camera_data.camera_position.z, 0.0f};

                    math::m4x4 viewProj = inputs.camera_data.proj_matrix * inputs.camera_data.view_matrix;
                    // 2026-09-16（audit V1）: 此前是 view*proj（与 Deferred
                    // /Forward 的 proj*view 契约相反），反投影出的世界位置不
                    // 对应 GBuffer——雾的坐标/阴影/相位全部被污染。
                    math::m4x4 invViewProj = rhi::math::Inverse(viewProj);
                    fp.InvViewProj[0] = invViewProj.columns[0];
                    fp.InvViewProj[1] = invViewProj.columns[1];
                    fp.InvViewProj[2] = invViewProj.columns[2];
                    fp.InvViewProj[3] = invViewProj.columns[3];

                    // Frustum（雾体自身范围——只定义 froxel 切片分布，不参与
                    // scene depth 解码）
                    float near_p = config_.near_plane;
                    float far_p  = config_.far_plane;
                    fp.FrustumParams = {near_p, far_p, log2f(far_p / near_p),
                                        1.0f / (float)config_.grid_depth};

                    // 真实相机 near/far（audit V3）: 从 proj 元素反解。LH [0,1]
                    // 深度投影：P[2].z=f/(f-n)，P[3].z=-nf/(f-n)。
                    {
                        const math::m4x4& P = inputs.camera_data.proj_matrix;
                        float a = P.columns[2].z;
                        float b = -P.columns[3].z; // = n*f/(f-n) > 0
                        float camNear = (a > 1e-6f) ? b / a : 0.1f;
                        float camFar = (a > 1.000001f) ? b / (a - 1.0f) : 1000.0f;
                        fp.FrustumParams2 = {camNear, camFar,
                                             config_.density_uniform_debug, 0.0f};
                    }

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
                                        config_.noise_speed, (float)shadowMode};

                    // WP1/WP2/WP3: march 步数 / SPGI 环境源 / 时域混合。
                    // override 优先（门禁 G5 双相位免重建切换），否则用 config；
                    // SPGI 句柄无效时环境源强制回退硬编码（ambientSrc=0）。
                    {
                        const u32 marchSteps = (march_steps_override_ != 0xFFFFFFFFu)
                            ? march_steps_override_ : config_.march_steps;
                        u32 ambientSrc = (ambient_spgi_override_ != 0xFFFFFFFFu)
                            ? ambient_spgi_override_ : config_.ambient_source_spgi;
                        if (inputs.spgi_ambient_tex == handles::INVALID_RESOURCE)
                            ambientSrc = 0;
                        const float blend = temporal_enabled_ ? config_.temporal_blend : 0.0f;
                        fp.VolumeParams4 = {(float)marchSteps, config_.march_distance,
                                            (float)ambientSrc, blend};
                    }

                    // WP2: 当前帧 view-projection（世界→屏幕采 SPGI 辐照）
                    fp.ViewProj[0] = viewProj.columns[0];
                    fp.ViewProj[1] = viewProj.columns[1];
                    fp.ViewProj[2] = viewProj.columns[2];
                    fp.ViewProj[3] = viewProj.columns[3];

                    // WP3: 上一帧 inverse view-projection（首帧用当前帧 =
                    // 恒等重投影；体积雾低频 + 邻域钳制兜底切镜鬼影）
                    {
                        const math::m4x4& prev = has_prev_camera_
                            ? prev_inv_view_proj_ : invViewProj;
                        fp.PrevInvViewProj[0] = prev.columns[0];
                        fp.PrevInvViewProj[1] = prev.columns[1];
                        fp.PrevInvViewProj[2] = prev.columns[2];
                        fp.PrevInvViewProj[3] = prev.columns[3];
                    }

                    // WP5 云原语：局部云体密度源（config 默认关）
                    fp.CloudA = {config_.cloud_enable, config_.cloud_radius,
                                 config_.cloud_density, config_.cloud_noise_freq};
                    fp.CloudB = {config_.cloud_center[0], config_.cloud_center[1],
                                 config_.cloud_center[2], config_.cloud_wind};

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

                    // Shadow VP matrices — P2-B: 4 级联。优先用新输入
                    // shadow_matrices[0..3]（与 shadow_moments 同源同帧）；
                    // 旧调用方未填充（全零）时 0/1 级联回退 legacy 字段，
                    // 保持既有 Metal/旧路径行为。
                    for (u32 c = 0; c < 4; ++c) {
                        const math::m4x4* src = &inputs.shadow_matrices[c];
                        const bool unpopulated =
                            src->columns[0].x == 0.0f && src->columns[1].y == 0.0f &&
                            src->columns[2].z == 0.0f && src->columns[3].w == 0.0f;
                        if (unpopulated && c < 2) {
                            src = (c == 0) ? &inputs.shadow_matrix0 : &inputs.shadow_matrix1;
                        }
                        fp.ShadowVP[c][0] = src->columns[0];
                        fp.ShadowVP[c][1] = src->columns[1];
                        fp.ShadowVP[c][2] = src->columns[2];
                        fp.ShadowVP[c][3] = src->columns[3];
                    }

                    *mapped = fp;
                    device_->UnmapBuffer(params_cb_[outIdx]);

                    // WP3: 记录本帧相机供下一帧重投影（首帧后 prev 有效）
                    prev_inv_view_proj_ = invViewProj;
                    has_prev_camera_ = true;

                    // P1（reaudit §4.4）: 一次性诊断——SDF 级联覆盖 vs 相机、
                    // 阴影模式、级联数。配合 FROXEL_DUMP 读回定位密度清零环节。
                    static bool s_froxDiagLogged = false;
                    // 2026-09-30: 追加周期打印（每 45 帧）——门禁 G5 双相位的
                    // march 切换（396+ 帧）需要确认 CB 实际值。
                    const u32 diagFrame = inputs.camera_data.frame_index;
                    if ((!s_froxDiagLogged || diagFrame == 350u || diagFrame % 45u == 0u)
                        && std::getenv("FROXEL_DUMP")) {
                        if (diagFrame == 350u) s_froxDiagLogged = true;
                        s_froxDiagLogged = true;
                        PRIMAL_LOG_STREAM(Info, "Engine")
                            << "[FROXEL_DIAG] frame=" << inputs.camera_data.frame_index
                            << " shadowMode=" << shadowMode
                            << " march=" << (int)fp.VolumeParams4.x
                            << " ambientSrc=" << (int)fp.VolumeParams4.z
                            << " temporal=" << fp.VolumeParams4.w
                            << " cascades=" << inputs.shadow_cascade_count
                            << " sdfCount=" << fp.SdfResolutions.w
                            << " sdfRes=(" << fp.SdfResolutions.x << "," << fp.SdfResolutions.y
                            << "," << fp.SdfResolutions.z << ")"
                            << " cam=(" << fp.CameraPos.x << "," << fp.CameraPos.y << "," << fp.CameraPos.z << ")"
                            << " frustum=(fog " << fp.FrustumParams.x << "," << fp.FrustumParams.y << ")"
                            << " cam=(n " << fp.FrustumParams2.x << " f " << fp.FrustumParams2.y << ")"
                            << " uniform=" << fp.FrustumParams2.z
                            << " vol1=(ext=" << fp.VolumeParams1.x << " thr=" << fp.VolumeParams1.z
                            << " fade=" << fp.VolumeParams1.w << ")"
                            << " vol2=(base=" << fp.VolumeParams2.x << " hscale=" << fp.VolumeParams2.y << ")";
                        for (u32 c = 0; c < 3 && c < (u32)fp.SdfResolutions.w; ++c) {
                            PRIMAL_LOG_STREAM(Info, "Engine")
                                << "[FROXEL_DIAG] sdf" << c
                                << " origin=(" << fp.SdfOrigins[c].x << "," << fp.SdfOrigins[c].y
                                << "," << fp.SdfOrigins[c].z << ")"
                                << " extent=(" << fp.SdfExtents[c].x << "," << fp.SdfExtents[c].y
                                << "," << fp.SdfExtents[c].z << ")";
                        }
                    }
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
                    {6, DescriptorType::UniformBuffer, params_cb_[outIdx]},
                    {7, DescriptorType::SampledImage,  cloud_texture_},
                };
                UpdateDescriptorSet(device_, density_inject_ds_[outIdx], params, 8);

                // P2-B: froxel textures are module-owned (not RG-imported);
                // prime the write slot to UA — on the first rotation cycle the
                // tracked layout is UNDEFINED and the storage write would be
                // flagged (expects GENERAL, got UNDEFINED).
                {
                    ResourceBarrier primeBarrier{};
                    primeBarrier.resource = froxel_density_[outIdx];
                    primeBarrier.beforeState = ResourceState::UnorderedAccess;
                    primeBarrier.afterState = ResourceState::UnorderedAccess;
                    primeBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&primeBarrier, 1);
                }

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
                    {3, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                    // P2-B: light-space VSM moments（无阴影时为噪声占位，
                    // shadowMode=0 下 shader 不采样）
                    {4, DescriptorType::SampledImage,  momentTex[0]},
                    {5, DescriptorType::SampledImage,  momentTex[1]},
                    {6, DescriptorType::SampledImage,  momentTex[2]},
                    {7, DescriptorType::SampledImage,  momentTex[3]},
                    // WP2: 上一帧 SPGI 辐照（无效时 1x1 黑，CB ambient_source=0）
                    {8, DescriptorType::SampledImage,  ambientTex},
                };
                UpdateDescriptorSet(device_, light_integrate_ds_[outIdx], params, 9);

                // P2-B: scatter 写入槽位同样先转 UA（见上方 density 注释；
                // 轮转槽位上一帧可能停在 SRO——上一帧作为 resolve 采样源）。
                {
                    ResourceBarrier primeBarrier{};
                    primeBarrier.resource = froxel_scatter_[outIdx];
                    primeBarrier.beforeState = ResourceState::UnorderedAccess;
                    primeBarrier.afterState = ResourceState::UnorderedAccess;
                    primeBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&primeBarrier, 1);
                }

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
            // Sub-pass 3 (WP3): Temporal Reproject
            // integrate 输出 → 与上一帧混合结果做邻域钳制混合 → hist 纹理。
            // temporal_enabled_=0 或管线编译失败时整段旁路（resolve 直采
            // integrate 输出，行为与历史一致）。
            // ================================================================
            const bool temporalActive = temporal_enabled_ &&
                                        temporal_available_ &&
                                        reproject_pipeline_ != handles::INVALID_PIPELINE;
            if (temporalActive) {
                const u32 prevIdx = (outIdx + 2u) % 3u;   // 上一帧写过的 hist 槽
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  froxel_scatter_[outIdx]},
                    {1, DescriptorType::SampledImage,  froxel_scatter_hist_[prevIdx]},
                    {2, DescriptorType::StorageImage,   froxel_scatter_hist_[outIdx]},
                    {3, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
                };
                UpdateDescriptorSet(device_, reproject_ds_[outIdx], params, 4);

                // hist 写入槽位 prime 到 UA（轮转槽位上一帧停在 SRO）
                {
                    ResourceBarrier primeBarrier{};
                    primeBarrier.resource = froxel_scatter_hist_[outIdx];
                    primeBarrier.beforeState = ResourceState::UnorderedAccess;
                    primeBarrier.afterState = ResourceState::UnorderedAccess;
                    primeBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&primeBarrier, 1);
                }

                cmd->BindComputePipeline(reproject_pipeline_);
                const DescriptorSetHandle sets[] = { reproject_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, reproject_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (config_.grid_width  + 3) / 4;
                u32 gy = (config_.grid_height + 3) / 4;
                u32 gz = (config_.grid_depth  + 3) / 4;
                cmd->Dispatch(gx, gy, gz);

                // Barrier: hist UAV -> SRV (resolve 采样源)
                {
                    ResourceBarrier barrier{};
                    barrier.resource = froxel_scatter_hist_[outIdx];
                    barrier.beforeState = ResourceState::UnorderedAccess;
                    barrier.afterState = ResourceState::ShaderResource;
                    barrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&barrier, 1);
                }
            }

            // ================================================================
            // Sub-pass 4: Resolve
            // temporal 开启时采混合结果（hist），否则采 integrate 输出——
            // 与 GetScatter3DTexture 的门禁读回保持同源。
            // ================================================================
            if (resolve_pipeline_ != handles::INVALID_PIPELINE) {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage,  temporalActive
                        ? froxel_scatter_hist_[outIdx] : froxel_scatter_[outIdx]},
                    {1, DescriptorType::SampledImage,  depthTex},
                    {2, DescriptorType::StorageImage,   scatter_texture_[outIdx]},
                    {3, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
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
