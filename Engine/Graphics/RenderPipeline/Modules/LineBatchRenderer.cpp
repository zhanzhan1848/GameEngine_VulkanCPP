#include "LineBatchRenderer.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "CommonHeaders.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace primal::graphics {

using namespace rhi;

LineBatchRenderer::~LineBatchRenderer() {
    Shutdown();
}

bool LineBatchRenderer::Initialize(RHIDeviceBase* device) {
    if (initialized_) return true;
    device_ = device;

    // Load shaders
    const std::filesystem::path shader_path =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "Metal/shaders/Forward/Line.metal";

    std::ifstream file(shader_path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[LineBatchRenderer] Failed to open shader: " << shader_path.string() << std::endl;
        return false;
    }
    const size_t file_size = static_cast<size_t>(file.tellg());
    std::vector<char> shader_data(file_size);
    file.seekg(0);
    file.read(shader_data.data(), file_size);
    file.close();

    vertex_shader_ = device_->CreateShader(shader_data.data(), shader_data.size(),
                                            ShaderStage::Vertex, "line_vs");
    pixel_shader_ = device_->CreateShader(shader_data.data(), shader_data.size(),
                                           ShaderStage::Pixel, "line_fs");

    if (vertex_shader_ == handles::INVALID_SHADER || pixel_shader_ == handles::INVALID_SHADER) {
        std::cerr << "[LineBatchRenderer] Failed to create shaders." << std::endl;
        return false;
    }

    // Pipeline layout with no descriptor sets (view_proj via push constants)
    PipelineLayoutDesc pl_desc{};
    pl_desc.setLayoutCount = 0;
    pl_desc.setLayouts     = nullptr;
    // Push constant range: 64 bytes = float4x4 view_proj at binding 0
    pl_desc.pushConstantRangeCount = 1;
    PushConstantRange pc_range;
    pc_range.stageFlags = ShaderStage::Vertex;
    pc_range.offset     = 2;
    pc_range.size       = 64; // sizeof(float4x4)
    pl_desc.pushConstantRanges = &pc_range;

    layout_ = device_->CreatePipelineLayout(pl_desc);
    if (layout_ == handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[LineBatchRenderer] Failed to create pipeline layout." << std::endl;
        return false;
    }

    initialized_ = true;
    return true;
}

bool LineBatchRenderer::create_pipeline(DataFormat target_format) {
    if (target_format == DataFormat::Unknown) return false;
    if (pipeline_ != handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(pipeline_);
        pipeline_ = handles::INVALID_PIPELINE;
    }

    // The editor renders into RGBA16F and later blits into a BGRA8 MTKView.
    // Build this PSO for the render target that is actually bound instead of
    // hard-coding the drawable format.
    GraphicsPipelineDesc p_desc;
    p_desc.vertexShader   = vertex_shader_;
    p_desc.pixelShader    = pixel_shader_;
    p_desc.layout         = layout_;
    p_desc.topology       = PrimitiveTopology::LineList;

    p_desc.renderTargetCount     = 1;
    p_desc.renderTargetFormats[0] = target_format;
    p_desc.depthStencilFormat    = DataFormat::D32_Float;

    p_desc.enableDepthTest  = true;
    p_desc.enableDepthWrite = false;
    p_desc.depthFunc        = ComparisonFunc::Less;

    p_desc.cullMode  = CullMode::None;
    p_desc.fillMode  = FillMode::Solid;
    p_desc.enableBlend = false;

    pipeline_ = device_->CreateGraphicsPipeline(p_desc);
    if (pipeline_ == handles::INVALID_PIPELINE) {
        std::cerr << "[LineBatchRenderer] Failed to create pipeline." << std::endl;
        return false;
    }

    pipeline_target_format_ = target_format;
    return true;
}

void LineBatchRenderer::Shutdown() {
    if (!initialized_) return;

    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(vertex_buffer_);
        vertex_buffer_ = handles::INVALID_RESOURCE;
    }
    if (pipeline_ != handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(pipeline_);
        pipeline_ = handles::INVALID_PIPELINE;
    }
    pipeline_target_format_ = DataFormat::Unknown;
    if (layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(layout_);
        layout_ = handles::INVALID_PIPELINE_LAYOUT;
    }

    initialized_ = false;
}

void LineBatchRenderer::BeginFrame() {
    line_vertices_.clear();
}

void LineBatchRenderer::AddLines(const math::v3* vertices, u32 vertex_count, u32 rgb) {
    if (!vertices || vertex_count == 0) return;
    line_vertices_.reserve(line_vertices_.size() + vertex_count);
    for (u32 i = 0; i < vertex_count; ++i) {
        line_vertices_.push_back({vertices[i].x, vertices[i].y, vertices[i].z, rgb});
    }
}

void LineBatchRenderer::AddLine(const math::v3& a, const math::v3& b, u32 rgb) {
    line_vertices_.push_back({a.x, a.y, a.z, rgb});
    line_vertices_.push_back({b.x, b.y, b.z, rgb});
}

void LineBatchRenderer::Render(RHICommandBuffer* cmd, const math::m4x4& view_proj,
                               DataFormat target_format) {
    if (!initialized_ || line_vertices_.empty()) return;
    if ((pipeline_ == handles::INVALID_PIPELINE || pipeline_target_format_ != target_format) &&
        !create_pipeline(target_format)) return;

    const u32 vertex_count = static_cast<u32>(line_vertices_.size());

    // Ensure GPU buffer is large enough
    ensure_buffer(vertex_count);

    // Upload vertex data
    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(vertex_buffer_);
        if (mapped) {
            memcpy(mapped, line_vertices_.data(), vertex_count * sizeof(LineVertex));
            device_->UnmapBuffer(vertex_buffer_);
        }
    }

    // Bind pipeline and push view_proj
    cmd->BindGraphicsPipeline(pipeline_);
    cmd->PushConstants(layout_, ShaderStage::Vertex, 2, sizeof(math::m4x4), &view_proj);

    // Bind vertex buffer at slot 1 (matches shader [[buffer(1)]])
    const u64 offset = 0;
    cmd->BindVertexBuffers(1, 1, &vertex_buffer_, &offset);

    // Draw
    cmd->Draw(vertex_count, 0, 1, 0);
}

void LineBatchRenderer::ensure_buffer(u32 required_vertices) {
    const u64 required_bytes = static_cast<u64>(required_vertices) * sizeof(LineVertex);

    if (buffer_capacity_ >= required_vertices && vertex_buffer_ != handles::INVALID_RESOURCE) return;

    // Create new buffer
    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(vertex_buffer_);
    }

    BufferDesc desc{};
    desc.size          = required_bytes;
    desc.type          = BufferType::Vertex;
    desc.usage         = GPUMemoryUsage::Dynamic;
    desc.memoryUsage   = GPUMemoryUsage::Dynamic;
    desc.vertex.vertexCount  = required_vertices;
    desc.vertex.vertexStride = sizeof(LineVertex);

    vertex_buffer_  = device_->CreateBuffer(desc);
    buffer_capacity_ = required_vertices;
}

} // namespace primal::graphics
