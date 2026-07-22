#include "VolumeRenderer.h"
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

namespace {

// ============================================================================
// Descriptor update helper (shared pattern with VolumePass)
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
// Shader source loader with #include resolution (same as VolumePass)
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
        std::cerr << "[VolumeRenderer] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, VOLUME_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// CPU-side Worley noise generation
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

} // anonymous namespace

// ============================================================================
// VolumeRenderer Implementation
// ============================================================================

VolumeRenderer::~VolumeRenderer() {
    Shutdown();
}

bool VolumeRenderer::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    if (initialized_) return true;

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

    CreatePipeline();
    if (pipeline_ == handles::INVALID_PIPELINE) {
        std::cerr << "[VolumeRenderer] Pipeline creation failed" << std::endl;
        return false;
    }

    CreateDescriptorSets();
    CreateConstantBuffers();
    CreateNoiseTexture();
    CreateOutputTextures();

    initialized_ = true;
    std::cout << "[VolumeRenderer] Initialized (" << render_width << "x" << render_height << ")"
              << " pipeline=" << (pipeline_ != handles::INVALID_PIPELINE ? "OK" : "FAIL")
              << " noise=" << (noise_texture_ != handles::INVALID_RESOURCE ? "OK" : "FAIL")
              << std::endl;
    return true;
}

void VolumeRenderer::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

void VolumeRenderer::CreatePipeline() {
    // Load shader source with #include resolution
    auto code = LoadShaderBytecode("VolumeObjectShader");
    if (code.empty()) {
        std::cerr << "[VolumeRenderer] Failed to load VolumeObjectShader" << std::endl;
        return;
    }

    auto vs = device_->CreateShader(code.data(), code.size(), ShaderStage::Vertex, "volume_object_vertex");
    auto fs = device_->CreateShader(code.data(), code.size(), ShaderStage::Pixel, "volume_object_fragment");

    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
        std::cerr << "[VolumeRenderer] Shader compilation failed: vs="
                  << (vs == handles::INVALID_SHADER ? "FAIL" : "OK")
                  << " fs=" << (fs == handles::INVALID_SHADER ? "FAIL" : "OK")
                  << std::endl;
        return;
    }

    // Descriptor set layout: 4 textures + 2 uniform buffers
    // No depth_buffer texture — proxy cube depth test via depth attachment handles occlusion
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // noise_tex
            {1, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // sdf_cascade_0
            {2, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // sdf_cascade_1
            {3, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // sdf_cascade_2
            // Buffers (Metal: separate namespace from textures)
            {0, DescriptorType::UniformBuffer,  1, ShaderStage::Vertex | ShaderStage::Pixel, nullptr},
            {1, DescriptorType::UniformBuffer,  1, ShaderStage::Vertex, nullptr},
        };
        DescriptorSetLayoutDesc layoutDesc{6, bindings};
        set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // Pipeline layout
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &set_layout_;
        pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Graphics pipeline
    {
        GraphicsPipelineDesc pDesc;
        pDesc.vertexShader = vs;
        pDesc.pixelShader  = fs;
        pDesc.layout       = pipeline_layout_;

        // Back-face culling — only render front faces of the proxy cube.
        // Each screen pixel gets exactly one fragment → one ray march through the volume.
        // Use CullMode::Front if camera can be inside the volume.
        pDesc.cullMode = CullMode::Back;

        // No depth test — volume scatter is translucent, composited by FusionComposite.
        // Depth test can't be used here because the render runs before GBuffer pass populates depth.
        pDesc.enableDepthTest  = false;
        pDesc.enableDepthWrite = false;

        // Blend: One / OneMinusSrcAlpha for color, overwrite alpha
        // Color: result = scatter * 1 + dst_color * (1 - alpha)
        // Alpha: result = src_alpha (overwrite, no blend)
        // This preserves transmittance in the alpha channel for FusionComposite:
        //   fusion = (scene + indirect) * vol.a + vol.rgb
        pDesc.enableBlend = true;
        pDesc.srcColorBlendFactor = BlendFactor::One;
        pDesc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        pDesc.colorBlendOp        = BlendOp::Add;
        pDesc.srcAlphaBlendFactor  = BlendFactor::One;
        pDesc.dstAlphaBlendFactor  = BlendFactor::Zero;
        pDesc.alphaBlendOp         = BlendOp::Add;

        // Render target: RGBA16_Float (HDR, pre-tone-map)
        pDesc.renderTargetCount    = 1;
        pDesc.renderTargetFormats[0] = DataFormat::RGBA16_Float;

        pipeline_ = device_->CreateGraphicsPipeline(pDesc);
    }
}

void VolumeRenderer::CreateDescriptorSets() {
    for (int i = 0; i < 3; i++) {
        DescriptorSetDesc dsDesc{set_layout_};
        descriptor_sets_[i] = device_->CreateDescriptorSet(dsDesc);
    }
}

void VolumeRenderer::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        // VolumeParams buffer
        BufferDesc desc{};
        desc.size = sizeof(VolumeParams) + 64;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_[i] = device_->CreateBuffer(desc);

        // VolumeTransform buffer (WorldMatrix + ViewProjMatrix)
        BufferDesc tDesc{};
        tDesc.size = sizeof(math::m4x4) * 2 + 64;
        tDesc.type = BufferType::Constant;
        tDesc.usage = GPUMemoryUsage::Dynamic;
        tDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        transform_cb_[i] = device_->CreateBuffer(tDesc);
    }
}

void VolumeRenderer::CreateNoiseTexture() {
    // 128³ R32_Float 3D noise texture, CPU-filled with FBM Worley
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
    texDesc.name = "VolumeRendererNoise3D";
    noise_texture_ = device_->CreateTexture(texDesc);

    if (noise_texture_ == handles::INVALID_RESOURCE) {
        std::cerr << "[VolumeRenderer] Failed to create noise texture" << std::endl;
        return;
    }

    // Generate noise data
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

    // Upload via staging buffer (GPUMemoryUsage::Unknown to bypass pool corruption)
    BufferDesc stagingDesc{};
    stagingDesc.size = NOISE_VOXELS * sizeof(float);
    stagingDesc.memoryUsage = GPUMemoryUsage::Unknown;
    stagingDesc.name = "VolumeRendererNoiseStaging";
    auto stagingBuffer = device_->CreateBuffer(stagingDesc);

    if (stagingBuffer == handles::INVALID_RESOURCE) {
        std::cerr << "[VolumeRenderer] Failed to create noise staging buffer" << std::endl;
        return;
    }

    float* mapped = static_cast<float*>(device_->MapBuffer(stagingBuffer));
    if (!mapped) {
        std::cerr << "[VolumeRenderer] Failed to map noise staging buffer" << std::endl;
        device_->DestroyBuffer(stagingBuffer);
        return;
    }

    memcpy(mapped, noiseData.data(), NOISE_VOXELS * sizeof(float));
    device_->UnmapBuffer(stagingBuffer);

    auto cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
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
    device_->DestroyBuffer(stagingBuffer);

    std::cout << "[VolumeRenderer] Noise texture created (128³ FBM Worley)" << std::endl;
}

void VolumeRenderer::CreateOutputTextures() {
    for (int i = 0; i < 3; i++) {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        desc.memoryUsage = GPUMemoryUsage::Static;
        scatter_texture_[i] = device_->CreateTexture(desc);
    }
}

rhi::ResourceHandle VolumeRenderer::GetScatterTexture(u32 frame_index) const {
    return scatter_texture_[frame_index % 3];
}

void VolumeRenderer::Render(rhi::RHICommandBuffer* cmd,
                            const VolumeCameraData& camera_data,
                            u32 frame_index,
                            u32 width, u32 height) {
    if (!initialized_ || !cmd) return;

    u32 outIdx = frame_index % 3;

    // ---- Update VolumeParams constant buffer (buffer 0) ----
    math::m4x4 viewProj;
    {
        auto* mapped = static_cast<VolumeParams*>(device_->MapBuffer(params_cb_[outIdx]));
        if (!mapped) return;

        VolumeParams p{};
        p.VolumeOrigin = {test_volume_origin_.x, test_volume_origin_.y, test_volume_origin_.z, 0.0f};
        p.VolumeExtent = {test_volume_extent_.x, test_volume_extent_.y, test_volume_extent_.z, 0.0f};

        p.CameraPos = {camera_data.camera_position.x, camera_data.camera_position.y,
                       camera_data.camera_position.z, 0.0f};

        viewProj = camera_data.proj_matrix * camera_data.view_matrix;
        math::m4x4 invViewProj = rhi::math::Inverse(viewProj);
        p.InvViewProj[0] = invViewProj.columns[0];
        p.InvViewProj[1] = invViewProj.columns[1];
        p.InvViewProj[2] = invViewProj.columns[2];
        p.InvViewProj[3] = invViewProj.columns[3];

        p.StepSize         = 0.5f;
        p.MaxDistance       = 200.0f;
        p.ExtinctionScale   = 2.0f;
        p.ScatteringAlbedo  = 0.8f;
        p.DensityThreshold  = 0.01f;
        p.DensityFadeRange  = 10.0f;
        p.MaxSteps          = 64;
        p.FrameIndex        = frame_index;
        p.ScreenWidth       = width;
        p.ScreenHeight      = height;
        p._pad0[0] = 0.0f;
        p._pad0[1] = 0.0f;

        for (int i = 0; i < 3; i++) {
            p.SdfOrigins[i]     = {0, 0, 0, 0};
            p.SdfVoxelSizes[i]  = {0, 0, 0, 0};
            p.SdfExtents[i]     = {0, 0, 0, 0};
            p.SdfResolutions[i] = 0;
        }
        p.SdfCascadeCount = 0;
        p._pad1[0] = 0.0f;

        p.LightDirection = {camera_data.light_direction.x, camera_data.light_direction.y,
                            camera_data.light_direction.z, 0.0f};
        p.LightColor = {camera_data.light_color.x, camera_data.light_color.y,
                        camera_data.light_color.z, 0.0f};

        *mapped = p;
        device_->UnmapBuffer(params_cb_[outIdx]);
    }

    // ---- Update VolumeTransform constant buffer (buffer 1) ----
    {
        struct VolumeTransform {
            math::m4x4 WorldMatrix;
            math::m4x4 ViewProjMatrix;
        };

        auto* mapped = static_cast<VolumeTransform*>(device_->MapBuffer(transform_cb_[outIdx]));
        if (!mapped) return;

        math::v3 center = test_volume_origin_ + test_volume_extent_ * 0.5f;
        mapped->WorldMatrix = math::m4x4{
            math::v4{test_volume_extent_.x, 0, 0, 0},
            math::v4{0, test_volume_extent_.y, 0, 0},
            math::v4{0, 0, test_volume_extent_.z, 0},
            math::v4{center.x, center.y, center.z, 1}
        };
        mapped->ViewProjMatrix = viewProj;

        device_->UnmapBuffer(transform_cb_[outIdx]);
    }

    // ---- Update descriptor set ----
    {
        DescriptorData params[] = {
            {0, DescriptorType::SampledImage,  noise_texture_},
            {1, DescriptorType::SampledImage,  noise_texture_},
            {2, DescriptorType::SampledImage,  noise_texture_},
            {3, DescriptorType::SampledImage,  noise_texture_},
            {0, DescriptorType::UniformBuffer,  params_cb_[outIdx]},
            {1, DescriptorType::UniformBuffer,  transform_cb_[outIdx]},
        };
        UpdateDescriptorSet(device_, descriptor_sets_[outIdx], params, 6);
    }

    // ---- Render pass: clear scatter texture, draw proxy cube ----
    {
        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture  = scatter_texture_[outIdx];
        rpDesc.colorAttachments[0].loadOp   = LoadAction::Clear;
        rpDesc.colorAttachments[0].clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 1.0f};
        rpDesc.colorAttachments[0].storeOp  = StoreAction::Store;
        cmd->BeginRenderPass(rpDesc);

        cmd->SetViewport({{0, 0}, {static_cast<float>(width), static_cast<float>(height)}, 0, 1});
        cmd->SetScissor({{0, 0}, {width, height}});

        cmd->BindGraphicsPipeline(pipeline_);

        const DescriptorSetHandle sets[] = { descriptor_sets_[outIdx] };
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipeline_layout_,
                                0, 1, sets, 0, nullptr);

        cmd->Draw(36, 0, 1, 0);

        cmd->EndRenderPass();
    }
}

} // namespace primal::graphics::volume
