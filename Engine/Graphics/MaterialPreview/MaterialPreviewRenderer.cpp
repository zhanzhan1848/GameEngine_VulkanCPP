#include "MaterialPreviewRenderer.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/ShaderIR/MaterialGraphToIR.h"
#include "Graphics/ShaderIR/MetalEmitter.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Content/ProceduralMesh.h"

#include <fstream>
#include <iostream>
#include <cmath>

namespace primal::graphics {

namespace {
bool ReadShaderFile(const char* relative_path, std::string& out) {
    std::string paths[2] = {
        std::string("Engine/Graphics/Metal/shaders/") + relative_path,
        std::string("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/") + relative_path,
    };
    for (auto& p : paths) {
        std::ifstream f(p, std::ios::ate | std::ios::binary);
        if (!f.is_open()) continue;
        size_t size = f.tellg();
        out.resize(size + 1);
        f.seekg(0);
        f.read(out.data(), size);
        out[size] = 0;
        return true;
    }
    return false;
}
} // namespace

bool MaterialPreviewRenderer::Initialize(rhi::RHIDeviceBase* device, u32 width, u32 height) {
    if (!device) return false;
    device_ = device;
    width_ = width;
    height_ = height;

    if (!CreateSamplers()) return false;
    if (!CreateDescriptorLayouts()) return false;
    if (!CreateShaders()) return false;
    if (!CreateBackgroundPipeline()) return false;
    if (!CreateBlitPipeline()) return false;
    if (!CreateRenderTargets()) return false;
    if (!CreateConstantBuffers()) return false;
    if (!CreateMeshes()) return false;

    UpdateGlobalDescriptorSets();
    UpdateBlitDescriptorSet();
    return true;
}

bool MaterialPreviewRenderer::CreateSamplers() {
    rhi::SamplerDesc desc{};
    desc.minFilter = rhi::FilterMode::Linear;
    desc.magFilter = rhi::FilterMode::Linear;
    desc.mipFilter = rhi::FilterMode::Linear;
    desc.addressU = rhi::TextureAddressMode::Wrap;
    desc.addressV = rhi::TextureAddressMode::Wrap;
    desc.addressW = rhi::TextureAddressMode::Wrap;
    linear_sampler_ = device_->CreateSampler(desc);
    return linear_sampler_ != rhi::handles::INVALID_RESOURCE;
}

bool MaterialPreviewRenderer::CreateDescriptorLayouts() {
    // Global layout: binding 0 = ViewData, binding 1 = SceneData
    {
        utl::vector<rhi::DescriptorSetLayoutBinding> bindings(2);
        bindings[0].binding = 0;
        bindings[0].descriptorType = rhi::DescriptorType::UniformBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;
        bindings[1].binding = 1;
        bindings[1].descriptorType = rhi::DescriptorType::UniformBuffer;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = bindings.data();
        desc.bindingCount = static_cast<u32>(bindings.size());
        global_layout_ = device_->CreateDescriptorSetLayout(desc);
        if (global_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc plDesc{};
        plDesc.setLayouts = &global_layout_;
        plDesc.setLayoutCount = 1;
        global_pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
        if (global_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Material layout: 4 textures + 1 sampler
    {
        utl::vector<rhi::DescriptorSetLayoutBinding> bindings(5);
        for (u32 i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = rhi::DescriptorType::SampledImage;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = rhi::ShaderStage::Pixel;
        }
        bindings[4].binding = 4;
        bindings[4].descriptorType = rhi::DescriptorType::Sampler;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = rhi::ShaderStage::Pixel;

        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = bindings.data();
        desc.bindingCount = static_cast<u32>(bindings.size());
        material_layout_ = device_->CreateDescriptorSetLayout(desc);
        if (material_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc plDesc{};
        rhi::DescriptorSetLayoutHandle layouts[2] = {global_layout_, material_layout_};
        plDesc.setLayouts = layouts;
        plDesc.setLayoutCount = 2;
        material_pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
        if (material_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Background pipeline layout (no descriptor sets)
    {
        rhi::PipelineLayoutDesc plDesc{};
        plDesc.setLayouts = nullptr;
        plDesc.setLayoutCount = 0;
        background_pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
        if (background_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Blit layout: 1 SampledImage at binding 0 (pixel stage). Sampler is constexpr in shader.
    {
        rhi::DescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = rhi::DescriptorType::SampledImage;
        binding.descriptorCount = 1;
        binding.stageFlags = rhi::ShaderStage::Pixel;

        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = &binding;
        desc.bindingCount = 1;
        blit_layout_ = device_->CreateDescriptorSetLayout(desc);
        if (blit_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc plDesc{};
        plDesc.setLayouts = &blit_layout_;
        plDesc.setLayoutCount = 1;
        blit_pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
        if (blit_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }
    return true;
}

bool MaterialPreviewRenderer::CreateShaders() {
    std::string src;
    if (!ReadShaderFile("Preview/PreviewVertex.metal", src)) {
        std::cerr << "[MaterialPreview] Failed to load PreviewVertex.metal\n";
        return false;
    }

    preview_vs_ = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Vertex, "previewVertexMain");
    background_vs_ = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Vertex, "fullscreenTriangleVS");
    background_fs_ = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Pixel, "gradientFragment");
    blit_vs_ = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Vertex, "blitVertex");
    blit_fs_ = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Pixel, "blitFragment");

    return preview_vs_ != rhi::handles::INVALID_SHADER
        && background_vs_ != rhi::handles::INVALID_SHADER
        && background_fs_ != rhi::handles::INVALID_SHADER
        && blit_vs_ != rhi::handles::INVALID_SHADER
        && blit_fs_ != rhi::handles::INVALID_SHADER;
}

bool MaterialPreviewRenderer::CreateBackgroundPipeline() {
    rhi::GraphicsPipelineDesc desc{};
    desc.vertexShader = background_vs_;
    desc.pixelShader = background_fs_;
    desc.layout = background_pipeline_layout_;
    desc.topology = rhi::PrimitiveTopology::TriangleList;
    desc.cullMode = rhi::CullMode::None;
    desc.enableDepthTest = false;
    desc.enableDepthWrite = false;
    desc.renderTargetCount = 1;
    desc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
    // Even with depth test/write disabled, Metal requires the pipeline to declare
    // the depth attachment format matching the render pass's framebuffer.
    desc.depthStencilFormat = rhi::DataFormat::D32_Float;

    background_pipeline_ = device_->CreateGraphicsPipeline(desc);
    return background_pipeline_ != rhi::handles::INVALID_PIPELINE;
}

bool MaterialPreviewRenderer::CreateBlitPipeline() {
    rhi::GraphicsPipelineDesc desc{};
    desc.vertexShader = blit_vs_;
    desc.pixelShader = blit_fs_;
    desc.layout = blit_pipeline_layout_;
    desc.topology = rhi::PrimitiveTopology::TriangleList;
    desc.cullMode = rhi::CullMode::None;
    desc.enableDepthTest = false;
    desc.enableDepthWrite = false;
    desc.renderTargetCount = 1;
    desc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
    // Blit renders directly to the swap chain backbuffer which has no depth attachment.
    desc.depthStencilFormat = rhi::DataFormat::Unknown;

    blit_pipeline_ = device_->CreateGraphicsPipeline(desc);
    return blit_pipeline_ != rhi::handles::INVALID_PIPELINE;
}

void MaterialPreviewRenderer::UpdateBlitDescriptorSet() {
    if (blit_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(blit_set_);
        blit_set_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    if (color_target_ == rhi::handles::INVALID_RESOURCE) return;

    rhi::DescriptorSetDesc desc{};
    desc.layout = blit_layout_;
    blit_set_ = device_->CreateDescriptorSet(desc);
    if (blit_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) return;

    rhi::DescriptorImageInfo imgInfo{};
    imgInfo.imageView = color_target_;
    imgInfo.sampler = linear_sampler_;  // unused (sampler is constexpr in shader) but required by API
    imgInfo.imageLayout = rhi::ResourceState::ShaderResource;

    rhi::WriteDescriptorSet write{};
    write.dstSet = blit_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = rhi::DescriptorType::SampledImage;
    write.imageInfo = &imgInfo;
    device_->UpdateDescriptorSets(1, &write);
}

bool MaterialPreviewRenderer::CreateRenderTargets() {
    // Color target — match swap chain format (BGRA8_UNorm) so BlitTexture works
    {
        rhi::TextureDesc desc{};
        desc.size = rhi::math::u32v3{width_, height_, 1};
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = rhi::DataFormat::BGRA8_UNorm;
        desc.type = rhi::TextureType::Texture2D;
        desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        color_target_ = device_->CreateTexture(desc);
        if (color_target_ == rhi::handles::INVALID_RESOURCE) return false;
    }
    // Depth target
    {
        rhi::TextureDesc desc{};
        desc.size = rhi::math::u32v3{width_, height_, 1};
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = rhi::DataFormat::D32_Float;
        desc.type = rhi::TextureType::Texture2D;
        desc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        depth_target_ = device_->CreateTexture(desc);
        if (depth_target_ == rhi::handles::INVALID_RESOURCE) return false;
    }
    return true;
}

bool MaterialPreviewRenderer::CreateConstantBuffers() {
    rhi::BufferDesc viewDesc{};
    viewDesc.size = sizeof(ViewData);
    viewDesc.type = rhi::BufferType::Constant;
    viewDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    viewDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    viewDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);

    rhi::BufferDesc sceneDesc{};
    sceneDesc.size = sizeof(SceneData);
    sceneDesc.type = rhi::BufferType::Constant;
    sceneDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    sceneDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    sceneDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        view_cb_[i] = device_->CreateBuffer(viewDesc);
        if (view_cb_[i] == rhi::handles::INVALID_RESOURCE) return false;
        view_cb_mapped_[i] = device_->MapBuffer(view_cb_[i], 0, sizeof(ViewData));

        scene_cb_[i] = device_->CreateBuffer(sceneDesc);
        if (scene_cb_[i] == rhi::handles::INVALID_RESOURCE) return false;
        scene_cb_mapped_[i] = device_->MapBuffer(scene_cb_[i], 0, sizeof(SceneData));
    }
    return true;
}

bool MaterialPreviewRenderer::CreateMeshes() {
    using namespace primal::content;
    // 3D primitives. Sphere radius 0.8 keeps model comfortably framed at FOV 45°, eye (0,0,4.5).
    meshes_[static_cast<u8>(PreviewModelType::Sphere)] =
        RenderMesh::CreateFromAsset(device_, create_sphere_mesh(0.8f, 32, 16));
    meshes_[static_cast<u8>(PreviewModelType::Box)] =
        RenderMesh::CreateFromAsset(device_, create_box_mesh(1.4f, 1.4f, 1.4f));
    meshes_[static_cast<u8>(PreviewModelType::Cylinder)] =
        RenderMesh::CreateFromAsset(device_, create_cylinder_mesh(0.7f, 1.6f, 32));
    meshes_[static_cast<u8>(PreviewModelType::Cone)] =
        RenderMesh::CreateFromAsset(device_, create_cone_mesh(0.9f, 1.8f, 32));
    meshes_[static_cast<u8>(PreviewModelType::Torus)] =
        RenderMesh::CreateFromAsset(device_, create_torus_mesh(0.8f, 0.25f, 32, 16));
    meshes_[static_cast<u8>(PreviewModelType::Capsule)] =
        RenderMesh::CreateFromAsset(device_, create_capsule_mesh(0.6f, 1.2f, 24, 8));
    meshes_[static_cast<u8>(PreviewModelType::Teapot)] =
        RenderMesh::CreateFromAsset(device_, create_teapot_mesh(2.0f, 12));
    meshes_[static_cast<u8>(PreviewModelType::Plane)] =
        RenderMesh::CreateFromAsset(device_, create_plane_mesh(1.6f, 1.6f, 1, 1));

    // 2D preview quad (XY plane, +Z facing). Spans [-1,1]²; the orthographic
    // projection in Render() maps this to the full viewport.
    quad_xy_mesh_ = RenderMesh::CreateFromAsset(device_, create_quad_xy_mesh(2.0f, 2.0f));

    for (u8 i = 0; i < static_cast<u8>(PreviewModelType::Count); ++i) {
        if (!meshes_[i] || !meshes_[i]->IsValid()) {
            std::cerr << "[MaterialPreview] Failed to create preview mesh " << i << "\n";
            return false;
        }
    }
    if (!quad_xy_mesh_ || !quad_xy_mesh_->IsValid()) {
        std::cerr << "[MaterialPreview] Failed to create 2D quad mesh\n";
        return false;
    }
    return true;
}

bool MaterialPreviewRenderer::CreateMaterialPipeline(const std::string& metal_source) {
    if (material_fs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(material_fs_);
        material_fs_ = rhi::handles::INVALID_SHADER;
    }
    if (material_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(material_pipeline_);
        material_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }

    material_fs_ = device_->CreateShader(metal_source.data(), metal_source.size(),
                                         rhi::ShaderStage::Pixel, "fragmentMain");
    if (material_fs_ == rhi::handles::INVALID_SHADER) return false;

    rhi::GraphicsPipelineDesc desc{};
    desc.vertexShader = preview_vs_;
    desc.pixelShader = material_fs_;
    desc.layout = material_pipeline_layout_;
    desc.topology = rhi::PrimitiveTopology::TriangleList;
    desc.cullMode = rhi::CullMode::None;  // Disable culling for diagnostics
    desc.enableDepthTest = true;
    desc.enableDepthWrite = true;
    desc.depthFunc = rhi::ComparisonFunc::Less;
    desc.renderTargetCount = 1;
    desc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
    desc.depthStencilFormat = rhi::DataFormat::D32_Float;

    material_pipeline_ = device_->CreateGraphicsPipeline(desc);
    return material_pipeline_ != rhi::handles::INVALID_PIPELINE;
}

bool MaterialPreviewRenderer::SetMaterialGraph(const material_graph::MaterialGraph& graph) {
    shader_ir::MaterialGraphToIR translator;
    auto ir = translator.Translate(graph);
    if (!ir.success) {
        std::cerr << "[MaterialPreview] MaterialGraph translation failed\n";
        return false;
    }

    shader_ir::MetalEmitter emitter;
    auto output = emitter.Emit(ir.function, /*preview_mode=*/true);
    if (!output.success) {
        std::cerr << "[MaterialPreview] Metal emission failed: " << output.log << "\n";
        return false;
    }

    if (!CreateMaterialPipeline(output.source)) {
        std::cerr << "[MaterialPreview] Material pipeline creation failed\n";
        return false;
    }
    material_graph_set_ = true;
    return true;
}

void MaterialPreviewRenderer::SetPreviewModel(PreviewModelType model) {
    if (model < PreviewModelType::Count) current_model_ = model;
}

// --- Camera/light/background setters ---

void MaterialPreviewRenderer::SetCameraPosition(f32 x, f32 y, f32 z) {
    camera_position_ = rhi::math::v3{x, y, z};
}

void MaterialPreviewRenderer::SetCameraTarget(f32 x, f32 y, f32 z) {
    camera_target_ = rhi::math::v3{x, y, z};
}

void MaterialPreviewRenderer::SetCameraFov(f32 radians) {
    camera_fov_radians_ = radians;
}

void MaterialPreviewRenderer::SetRotationSpeed(f32 rad_per_sec) {
    rotation_speed_ = rad_per_sec;
}

void MaterialPreviewRenderer::SetLightDirection(f32 x, f32 y, f32 z) {
    rhi::math::v3 dir{x, y, z};
    // Normalize so callers can pass un-normalized vectors.
    f32 len2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (len2 > 1e-8f) {
        f32 inv = 1.0f / std::sqrt(len2);
        light_direction_ = rhi::math::v3{dir.x * inv, dir.y * inv, dir.z * inv};
    }
}

void MaterialPreviewRenderer::SetLightColor(f32 r, f32 g, f32 b) {
    light_color_ = rhi::math::v3{r, g, b};
}

void MaterialPreviewRenderer::SetLightIntensity(f32 intensity) {
    light_intensity_ = intensity;
}

void MaterialPreviewRenderer::SetBackgroundMode(PreviewBackgroundMode mode) {
    bg_mode_ = mode;
}

void MaterialPreviewRenderer::SetBackgroundColors(f32 top_r, f32 top_g, f32 top_b,
                                                   f32 bot_r, f32 bot_g, f32 bot_b) {
    bg_top_color_ = rhi::math::v3{top_r, top_g, top_b};
    bg_bot_color_ = rhi::math::v3{bot_r, bot_g, bot_b};
}

bool MaterialPreviewRenderer::SetPreviewMeshFromAsset(id::id_type geometry_content_id) {
    auto* new_mesh = RenderMesh::CreateFromAsset(device_, geometry_content_id);
    if (!new_mesh || !new_mesh->IsValid()) {
        std::cerr << "[MaterialPreview] SetPreviewMeshFromAsset: failed to load geometry id "
                  << geometry_content_id << "\n";
        delete new_mesh;
        return false;
    }
    if (override_mesh_) {
        override_mesh_->Destroy(device_);
        delete override_mesh_;
    }
    override_mesh_ = new_mesh;
    return true;
}

void MaterialPreviewRenderer::ClearPreviewMeshOverride() {
    if (override_mesh_) {
        override_mesh_->Destroy(device_);
        delete override_mesh_;
        override_mesh_ = nullptr;
    }
}

void MaterialPreviewRenderer::UpdateGlobalDescriptorSets() {
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        rhi::DescriptorSetDesc desc{};
        desc.layout = global_layout_;
        global_set_[i] = device_->CreateDescriptorSet(desc);
        if (global_set_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) continue;

        rhi::DescriptorBufferInfo bufInfo[2]{};
        bufInfo[0].buffer = view_cb_[i];
        bufInfo[0].offset = 0;
        bufInfo[0].range = sizeof(ViewData);
        bufInfo[1].buffer = scene_cb_[i];
        bufInfo[1].offset = 0;
        bufInfo[1].range = sizeof(SceneData);

        rhi::WriteDescriptorSet writes[2]{};
        writes[0].dstSet = global_set_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = rhi::DescriptorType::UniformBuffer;
        writes[0].bufferInfo = &bufInfo[0];
        writes[1].dstSet = global_set_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = rhi::DescriptorType::UniformBuffer;
        writes[1].bufferInfo = &bufInfo[1];
        device_->UpdateDescriptorSets(2, writes);
    }
}

rhi::ResourceHandle MaterialPreviewRenderer::Render(rhi::RHICommandBuffer* cmd, u32 frame_index, f32 dt) {
    if (!cmd || !device_) return rhi::handles::INVALID_RESOURCE;

    namespace rmath = rhi::math;
    const u32 fi = frame_index % FRAME_COUNT;

    rmath::m4x4 model;
    rmath::m4x4 view;
    rmath::m4x4 proj;
    rmath::v3   eye{};

    if (mode_2d_) {
        // Orthographic, no rotation, camera at origin looking down -Z. Quad spans [-1,1]².
        model = rmath::MatrixIdentity();
        view  = rmath::MatrixIdentity();
        const f32 aspect = f32(width_) / f32(height_);
        proj = rmath::CreateOrthographicMatrix(-aspect, aspect, -1.0f, 1.0f, -1.0f, 1.0f);
        eye  = rmath::v3{0.0f, 0.0f, 1.0f};
    } else {
        rotation_angle_ += rotation_speed_ * dt;
        model = rmath::CreateRotationMatrixY(rotation_angle_);
        eye   = camera_position_;
        const rmath::v3 target = camera_target_;
        const rmath::v3 up{0.0f, 1.0f, 0.0f};
        view = rmath::CreateLookAtMatrix(eye, target, up);
        proj = rmath::CreatePerspectiveMatrix(camera_fov_radians_,
                                              f32(width_) / f32(height_),
                                              0.1f, 100.0f);
    }

    const rmath::m4x4 viewProj = proj * view;
    const rmath::m4x4 invViewProj = rmath::Inverse(viewProj);

    // Write constant buffers
    if (view_cb_mapped_[fi]) {
        auto* vd = static_cast<ViewData*>(view_cb_mapped_[fi]);
        vd->viewProj = viewProj;
        vd->invViewProj = invViewProj;
        vd->prevViewProj = viewProj;
    }
    if (scene_cb_mapped_[fi]) {
        auto* sd = static_cast<SceneData*>(scene_cb_mapped_[fi]);
        sd->model = model;
        sd->lightDir = rmath::v4{light_direction_.x, light_direction_.y,
                                  light_direction_.z, 0.0f};
        // Bake intensity into lightColor so existing material graphs pick it up
        // without shader changes.
        sd->lightColor = rmath::v4{light_color_.x * light_intensity_,
                                    light_color_.y * light_intensity_,
                                    light_color_.z * light_intensity_,
                                    1.0f};
        sd->lightIntensity = light_intensity_;
        sd->time = rotation_angle_;
        sd->cameraPos = eye;
        sd->bgTopColor = rmath::v4{bg_top_color_.x, bg_top_color_.y, bg_top_color_.z, 0.0f};
        sd->bgBotColor = rmath::v4{bg_bot_color_.x, bg_bot_color_.y, bg_bot_color_.z, 0.0f};
        sd->bgMode = static_cast<f32>(bg_mode_);
    }

    // Viewport + scissor
    rhi::ViewportDesc viewport{};
    viewport.topLeft = rhi::math::v2{0.0f, 0.0f};
    viewport.size = rhi::math::v2{static_cast<f32>(width_), static_cast<f32>(height_)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    rhi::Rect scissor{};
    scissor.offset = rhi::math::s32v2{0, 0};
    scissor.extent = rhi::math::u32v2{width_, height_};

    // Pass 1: Background (gradient shader or solid clear color)
    {
        rhi::RenderPassDesc pass{};
        rhi::RenderPassDesc::Attachment colorAtt{};
        colorAtt.texture = color_target_;
        colorAtt.loadOp = rhi::LoadAction::Clear;
        colorAtt.storeOp = rhi::StoreAction::Store;
        if (bg_mode_ == PreviewBackgroundMode::SolidColor) {
            // Use the bottom gradient color as the solid fill.
            colorAtt.clearValue.color = rhi::math::v4{bg_bot_color_.x, bg_bot_color_.y,
                                                       bg_bot_color_.z, 1.0f};
        } else {
            // Gradient: clear with bot color, then draw gradient fullscreen triangle.
            colorAtt.clearValue.color = rhi::math::v4{bg_bot_color_.x, bg_bot_color_.y,
                                                       bg_bot_color_.z, 1.0f};
        }
        pass.colorAttachments.push_back(colorAtt);
        rhi::RenderPassDesc::Attachment depthAtt{};
        depthAtt.texture = depth_target_;
        depthAtt.loadOp = rhi::LoadAction::Clear;
        depthAtt.storeOp = rhi::StoreAction::Store;
        depthAtt.clearValue.depth = 1.0f;
        pass.depthAttachment = depthAtt;

        cmd->BeginRenderPass(pass);
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);

        if (bg_mode_ == PreviewBackgroundMode::Gradient) {
            // Push bg colors to the fragment shader via buffer(2).
            struct BgPushData {
                rhi::math::v3 topColor;
                f32 _pad0;
                rhi::math::v3 botColor;
                f32 _pad1;
            } bg_push{
                bg_top_color_, 0.0f,
                bg_bot_color_, 0.0f
            };
            cmd->PushConstants(background_pipeline_layout_,
                               rhi::ShaderStage::Pixel,
                               0, sizeof(BgPushData), &bg_push);
            // Draw the gradient fullscreen triangle.
            cmd->BindGraphicsPipeline(background_pipeline_);
            cmd->Draw(3, 0, 1, 0);
        }

        cmd->EndRenderPass();
    }

    // Pass 2: Material mesh (load color, clear depth)
    if (material_graph_set_ && material_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        rhi::RenderPassDesc pass{};
        rhi::RenderPassDesc::Attachment colorAtt{};
        colorAtt.texture = color_target_;
        colorAtt.loadOp = rhi::LoadAction::Load;
        colorAtt.storeOp = rhi::StoreAction::Store;
        pass.colorAttachments.push_back(colorAtt);
        rhi::RenderPassDesc::Attachment depthAtt{};
        depthAtt.texture = depth_target_;
        depthAtt.loadOp = rhi::LoadAction::Clear;
        depthAtt.storeOp = rhi::StoreAction::Store;
        depthAtt.clearValue.depth = 1.0f;
        pass.depthAttachment = depthAtt;

        cmd->BeginRenderPass(pass);
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);
        cmd->BindGraphicsPipeline(material_pipeline_);

        rhi::DescriptorSetHandle sets[1] = {global_set_[fi]};
        cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, material_pipeline_layout_,
                                 0, 1, sets, 0, nullptr);

        RenderMesh* mesh = override_mesh_ ? override_mesh_
                         : mode_2d_       ? quad_xy_mesh_
                                          : meshes_[static_cast<u8>(current_model_)];
        if (mesh) {
            mesh->Draw(cmd, 1, 0, 20);
        }
        cmd->EndRenderPass();
    }

    return color_target_;
}

void MaterialPreviewRenderer::BlitToTexture(rhi::RHICommandBuffer* cmd,
                                            rhi::ResourceHandle dst_texture,
                                            u32 dst_width, u32 dst_height) {
    if (!cmd || !device_) return;
    if (blit_pipeline_ == rhi::handles::INVALID_PIPELINE) return;
    if (color_target_ == rhi::handles::INVALID_RESOURCE) return;
    if (dst_texture == rhi::handles::INVALID_RESOURCE) return;
    if (blit_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) return;

    rhi::RenderPassDesc pass{};
    rhi::RenderPassDesc::Attachment colorAtt{};
    colorAtt.texture = dst_texture;
    colorAtt.loadOp = rhi::LoadAction::DontCare;  // overwrite every pixel
    colorAtt.storeOp = rhi::StoreAction::Store;
    pass.colorAttachments.push_back(colorAtt);

    cmd->BeginRenderPass(pass);

    rhi::ViewportDesc viewport{};
    viewport.topLeft = rhi::math::v2{0.0f, 0.0f};
    viewport.size = rhi::math::v2{static_cast<f32>(dst_width), static_cast<f32>(dst_height)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    rhi::Rect scissor{};
    scissor.offset = rhi::math::s32v2{0, 0};
    scissor.extent = rhi::math::u32v2{dst_width, dst_height};
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);

    cmd->BindGraphicsPipeline(blit_pipeline_);
    rhi::DescriptorSetHandle sets[1] = {blit_set_};
    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_pipeline_layout_,
                             0, 1, sets, 0, nullptr);
    cmd->Draw(3, 0, 1, 0);

    cmd->EndRenderPass();
}

void MaterialPreviewRenderer::Resize(u32 w, u32 h) {
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    if (color_target_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(color_target_);
        color_target_ = rhi::handles::INVALID_RESOURCE;
    }
    if (depth_target_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(depth_target_);
        depth_target_ = rhi::handles::INVALID_RESOURCE;
    }
    CreateRenderTargets();
    // color_target_ handle changed — rebind the blit descriptor set.
    UpdateBlitDescriptorSet();
}

void MaterialPreviewRenderer::Shutdown() {
    if (!device_) return;

    if (override_mesh_) {
        override_mesh_->Destroy(device_);
        delete override_mesh_;
        override_mesh_ = nullptr;
    }
    if (quad_xy_mesh_) {
        quad_xy_mesh_->Destroy(device_);
        delete quad_xy_mesh_;
        quad_xy_mesh_ = nullptr;
    }

    for (u8 i = 0; i < static_cast<u8>(PreviewModelType::Count); ++i) {
        if (meshes_[i]) {
            meshes_[i]->Destroy(device_);
            delete meshes_[i];
            meshes_[i] = nullptr;
        }
    }

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        if (view_cb_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->UnmapBuffer(view_cb_[i]);
            device_->DestroyBuffer(view_cb_[i]);
            view_cb_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (scene_cb_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->UnmapBuffer(scene_cb_[i]);
            device_->DestroyBuffer(scene_cb_[i]);
            scene_cb_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (global_set_[i] != rhi::handles::INVALID_DESCRIPTOR_SET) {
            device_->DestroyDescriptorSet(global_set_[i]);
            global_set_[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
        }
    }

    if (color_target_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(color_target_);
        color_target_ = rhi::handles::INVALID_RESOURCE;
    }
    if (depth_target_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(depth_target_);
        depth_target_ = rhi::handles::INVALID_RESOURCE;
    }
    if (background_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(background_pipeline_);
        background_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (blit_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(blit_pipeline_);
        blit_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (blit_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(blit_set_);
        blit_set_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    if (material_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(material_pipeline_);
        material_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (material_fs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(material_fs_);
        material_fs_ = rhi::handles::INVALID_SHADER;
    }
    if (preview_vs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(preview_vs_);
        preview_vs_ = rhi::handles::INVALID_SHADER;
    }
    if (background_vs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(background_vs_);
        background_vs_ = rhi::handles::INVALID_SHADER;
    }
    if (background_fs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(background_fs_);
        background_fs_ = rhi::handles::INVALID_SHADER;
    }
    if (blit_vs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(blit_vs_);
        blit_vs_ = rhi::handles::INVALID_SHADER;
    }
    if (blit_fs_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(blit_fs_);
        blit_fs_ = rhi::handles::INVALID_SHADER;
    }
    if (linear_sampler_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroySampler(linear_sampler_);
        linear_sampler_ = rhi::handles::INVALID_RESOURCE;
    }
    if (global_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(global_layout_);
        global_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (material_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(material_layout_);
        material_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (blit_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(blit_layout_);
        blit_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (global_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(global_pipeline_layout_);
        global_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (material_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(material_pipeline_layout_);
        material_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (background_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(background_pipeline_layout_);
        background_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (blit_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(blit_pipeline_layout_);
        blit_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }

    device_ = nullptr;
    material_graph_set_ = false;
}

} // namespace primal::graphics
