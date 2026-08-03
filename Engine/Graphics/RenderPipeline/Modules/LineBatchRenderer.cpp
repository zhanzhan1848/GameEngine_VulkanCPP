#include "LineBatchRenderer.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "CommonHeaders.h"
#include <fstream>
#include <iostream>
#include <vector>

namespace primal::graphics {

using namespace rhi;

// T4.6.5 part 16.4: local descriptor-write helper. Mirrors the pattern in
// ForwardSceneRenderer.cpp:83-118 — kept local to avoid expanding the RHI
// surface for one tiny debug-visualization pass.
namespace {
struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set,
                const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
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
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}
} // anonymous namespace

LineBatchRenderer::~LineBatchRenderer() {
    Shutdown();
}

bool LineBatchRenderer::Initialize(RHIDeviceBase* device) {
    if (initialized_) return true;
    if (!device) return false;

    device_ = device;
    const bool isVulkan = (device->GetPlatform() == RHIPlatform::Vulkan);

    // T4.6.5 part 16.4: Load SPIR-V (Vulkan) or .metal (Metal) shaders.
    ShaderHandle vs, fs;
    if (isVulkan) {
        // SPIR-V path: try relative, then dev-machine fallback.
        auto loadSpv = [](const char* relPath) -> std::vector<u8> {
            {
                std::ifstream f(relPath, std::ios::binary | std::ios::ate);
                if (f.is_open()) {
                    const size_t sz = static_cast<size_t>(f.tellg());
                    std::vector<u8> buf(sz);
                    f.seekg(0);
                    f.read(reinterpret_cast<char*>(buf.data()), sz);
                    return buf;
                }
            }
            const char* FALLBACK_ROOT = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/";
            std::string fb = std::string(FALLBACK_ROOT) + relPath;
            std::ifstream f(fb, std::ios::binary | std::ios::ate);
            if (f.is_open()) {
                const size_t sz = static_cast<size_t>(f.tellg());
                std::vector<u8> buf(sz);
                f.seekg(0);
                f.read(reinterpret_cast<char*>(buf.data()), sz);
                return buf;
            }
            return {};
        };
        auto vertBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Line.vert.spv");
        auto fragBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Line.frag.spv");
        if (vertBytes.empty() || fragBytes.empty()) {
            std::cerr << "[LineBatchRenderer] Failed to load Vulkan Line SPIR-V." << std::endl;
            return false;
        }
        vs = device_->CreateShader(vertBytes.data(), vertBytes.size(),
                                    ShaderStage::Vertex, "main");
        fs = device_->CreateShader(fragBytes.data(), fragBytes.size(),
                                    ShaderStage::Pixel, "main");
    } else {
        // Metal path — read .metal text, use named entry points.
        const char* shader_path =
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Forward/Line.metal";

        std::ifstream file(shader_path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[LineBatchRenderer] Failed to open shader: " << shader_path << std::endl;
            return false;
        }
        const size_t file_size = static_cast<size_t>(file.tellg());
        std::vector<char> shader_data(file_size);
        file.seekg(0);
        file.read(shader_data.data(), file_size);
        file.close();
        vs = device_->CreateShader(shader_data.data(), shader_data.size(),
                                    ShaderStage::Vertex, "line_vs");
        fs = device_->CreateShader(shader_data.data(), shader_data.size(),
                                    ShaderStage::Pixel, "line_fs");
    }

    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
        std::cerr << "[LineBatchRenderer] Failed to create shaders." << std::endl;
        return false;
    }

    // T4.6.5 part 16.4: Vulkan needs a descriptor set layout for the SSBO
    // (vertex buffer bound at set 0 / binding 1; matches Line.vert). Metal
    // path keeps the no-descriptor-set layout (uses BindVertexBuffers).
    DescriptorSetLayoutHandle set_layout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    if (isVulkan) {
        DescriptorSetLayoutBinding ssbo_binding{};
        ssbo_binding.binding = 1;
        ssbo_binding.descriptorType = DescriptorType::StorageBuffer;
        ssbo_binding.descriptorCount = 1;
        ssbo_binding.stageFlags = ShaderStage::Vertex;

        DescriptorSetLayoutDesc ds_desc{};
        ds_desc.bindingCount = 1;
        ds_desc.bindings = &ssbo_binding;
        set_layout = device_->CreateDescriptorSetLayout(ds_desc);
        if (set_layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            std::cerr << "[LineBatchRenderer] Failed to create descriptor set layout." << std::endl;
            return false;
        }
        set_layout_ = set_layout;

        // Allocate persistent descriptor set; the SSBO is rebound per-frame.
        DescriptorSetHandle ds = device_->CreateDescriptorSet({set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) {
            std::cerr << "[LineBatchRenderer] Failed to allocate descriptor set." << std::endl;
            return false;
        }
        descriptor_set_ = ds;
    }

    PipelineLayoutDesc pl_desc{};
    pl_desc.pushConstantRangeCount = 1;
    PushConstantRange pc_range;
    pc_range.stageFlags = ShaderStage::Vertex;
    pc_range.offset     = 0;
    pc_range.size       = 64; // sizeof(float4x4)
    pl_desc.pushConstantRanges = &pc_range;

    if (isVulkan) {
        pl_desc.setLayoutCount = 1;
        pl_desc.setLayouts = &set_layout;
    } else {
        pl_desc.setLayoutCount = 0;
        pl_desc.setLayouts = nullptr;
    }

    layout_ = device_->CreatePipelineLayout(pl_desc);
    if (layout_ == handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[LineBatchRenderer] Failed to create pipeline layout." << std::endl;
        return false;
    }

    GraphicsPipelineDesc p_desc;
    p_desc.vertexShader   = vs;
    p_desc.pixelShader    = fs;
    p_desc.layout         = layout_;
    p_desc.topology       = PrimitiveTopology::LineList;

    p_desc.renderTargetCount     = 1;
    p_desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
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

    std::cout << "[LineBatchRenderer] Initialized successfully."
              << (isVulkan ? " (Vulkan)" : "") << std::endl;
    initialized_ = true;
    return true;
}

void LineBatchRenderer::Shutdown() {
    if (!initialized_) return;

    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(vertex_buffer_);
        vertex_buffer_ = handles::INVALID_RESOURCE;
    }
    if (descriptor_set_ != handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(descriptor_set_);
        descriptor_set_ = handles::INVALID_DESCRIPTOR_SET;
    }
    if (set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(set_layout_);
        set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (pipeline_ != handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(pipeline_);
        pipeline_ = handles::INVALID_PIPELINE;
    }
    if (layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(layout_);
        layout_ = handles::INVALID_PIPELINE_LAYOUT;
    }

    initialized_ = false;
}

void LineBatchRenderer::BeginFrame() {
    line_vertices_.clear();
}

void LineBatchRenderer::AddLines(const math::v3* vertices, u32 vertex_count) {
    if (!vertices || vertex_count == 0) return;
    line_vertices_.insert(line_vertices_.end(), vertices, vertices + vertex_count);
}

void LineBatchRenderer::Render(RHICommandBuffer* cmd, const math::m4x4& view_proj) {
    if (!initialized_ || line_vertices_.empty()) return;

    const u32 vertex_count = static_cast<u32>(line_vertices_.size());

    // Ensure GPU buffer is large enough
    ensure_buffer(vertex_count);

    // Upload vertex data
    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(vertex_buffer_);
        if (mapped) {
            memcpy(mapped, line_vertices_.data(), vertex_count * sizeof(math::v3));
            device_->UnmapBuffer(vertex_buffer_);
        }
    }

    // Bind pipeline and push view_proj
    cmd->BindGraphicsPipeline(pipeline_);
    cmd->PushConstants(layout_, ShaderStage::Vertex, 0, sizeof(math::m4x4), &view_proj);

    const bool isVulkan = (device_->GetPlatform() == RHIPlatform::Vulkan);
    if (isVulkan) {
        // Write the SSBO to descriptor set binding 1, then bind the set.
        DescData params[] = {
            {1, DescriptorType::StorageBuffer, vertex_buffer_},
        };
        UpdateDesc(device_, descriptor_set_, params, 1);
        const DescriptorSetHandle sets[] = {descriptor_set_};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout_, 0, 1, sets, 0, nullptr);
    } else {
        // Metal path: bind vertex buffer at slot 1 (matches shader [[buffer(1)]]).
        const u64 offset = 0;
        cmd->BindVertexBuffers(1, 1, &vertex_buffer_, &offset);
    }

    // Draw
    cmd->Draw(vertex_count, 0, 1, 0);
}

void LineBatchRenderer::ensure_buffer(u32 required_vertices) {
    const u64 required_bytes = static_cast<u64>(required_vertices) * sizeof(math::v3);

    if (buffer_capacity_ >= required_vertices && vertex_buffer_ != handles::INVALID_RESOURCE) return;

    // Create new buffer
    if (vertex_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(vertex_buffer_);
    }

    const bool isVulkan = (device_->GetPlatform() == RHIPlatform::Vulkan);

    BufferDesc desc{};
    desc.size          = required_bytes;
    // T4.6.5 part 16.4: Vulkan reads vertices via SSBO (Line.vert binding 1).
    // Metal uses the Vertex buffer type.
    desc.type          = isVulkan ? BufferType::Structured : BufferType::Vertex;
    desc.usage         = GPUMemoryUsage::Dynamic;
    desc.memoryUsage   = GPUMemoryUsage::Dynamic;
    desc.vertex.vertexCount  = required_vertices;
    desc.vertex.vertexStride = sizeof(math::v3);

    vertex_buffer_  = device_->CreateBuffer(desc);
    buffer_capacity_ = required_vertices;
}

} // namespace primal::graphics
