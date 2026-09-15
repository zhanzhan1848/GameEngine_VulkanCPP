// ParticleFXLightPass.cpp — 见 ParticleFXLightPass.h。

#include "ParticleFXLightPass.h"
#include "ParticleFX/PfxTypes.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>

namespace primal::graphics {

namespace {

constexpr u32 kLightInstanceBytes = ParticleFXLightPass::instance_stride();

std::string load_light_shader_source() {
    const char* candidates[] = {
        "EngineTest/shaders/ParticleFXLight.metal",
        "shaders/ParticleFXLight.metal",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/ParticleFXLight.metal",
    };
    for (const char* path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) continue;
        std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!code.empty()) return code;
    }
    return {};
}

} // namespace

bool ParticleFXLightPass::initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    if (device_) return true;

    device_ = device;
        const bool isVulkan = (device->GetPlatform() == rhi::RHIPlatform::Vulkan);
    if (isVulkan) {
        // ---- Vulkan：加载预编译 SPIR-V（N2 对称路径）----
        auto loadSpvBinary = [](const char* relPath) -> std::vector<u8> {
            std::ifstream f0(relPath, std::ios::ate | std::ios::binary);
            if (f0.is_open()) {
                const std::streamsize sz = f0.tellg();
                f0.seekg(0, std::ios::beg);
                std::vector<u8> bytes(static_cast<size_t>(sz));
                f0.read(reinterpret_cast<char*>(bytes.data()), sz);
                return bytes;
            }
            std::string fb = std::string("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/") + relPath;
            std::ifstream f1(fb, std::ios::ate | std::ios::binary);
            if (f1.is_open()) {
                const std::streamsize sz = f1.tellg();
                f1.seekg(0, std::ios::beg);
                std::vector<u8> bytes(static_cast<size_t>(sz));
                f1.read(reinterpret_cast<char*>(bytes.data()), sz);
                return bytes;
            }
            return {};
        };
        std::vector<u8> vsb = loadSpvBinary("Engine/Graphics/Vulkan/Shaders/PFX/ParticleFXLight.vert.spv");
        std::vector<u8> fsb = loadSpvBinary("Engine/Graphics/Vulkan/Shaders/PFX/ParticleFXLight.frag.spv");
        if (vsb.empty() || fsb.empty()) {
            PRIMAL_LOG_STREAM(Info, "ParticleFX") << "light pass: SPIR-V not found, pass disabled" << std::endl;
            return true;
        }
        vertex_shader_ = device->CreateShader(vsb.data(), vsb.size(), rhi::ShaderStage::Vertex, "main");
        fragment_shader_ = device->CreateShader(fsb.data(), fsb.size(), rhi::ShaderStage::Pixel, "main");
        if (vertex_shader_ == rhi::handles::INVALID_SHADER || fragment_shader_ == rhi::handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "ParticleFX") << "light pass: Vulkan shader creation failed" << std::endl;
            return true;
        }
    } else {


    const std::string code = load_light_shader_source();
    if (code.empty()) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "light pass: shader source not found" << std::endl;
        return false;
    }
    vertex_shader_ = device->CreateShader(code.c_str(), code.size(), rhi::ShaderStage::Vertex,
                                          "pfx_light_vertex");
    fragment_shader_ = device->CreateShader(code.c_str(), code.size(), rhi::ShaderStage::Pixel,
                                            "pfx_light_fragment");
    if (vertex_shader_ == rhi::handles::INVALID_SHADER ||
        fragment_shader_ == rhi::handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "light pass: shader creation failed" << std::endl;
        return false;
    }

    } // Metal shader 路径

    rhi::DescriptorSetLayoutBinding bindings[] = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },   // light instances
        { 1, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Vertex },   // VP
    };
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = bindings;
    layout_desc.bindingCount = 2;
    descriptor_layout_ = device->CreateDescriptorSetLayout(layout_desc);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.setLayoutCount = 1;
    pl_desc.setLayouts = &descriptor_layout_;
    pipeline_layout_ = device->CreatePipelineLayout(pl_desc);

    rhi::GraphicsPipelineDesc desc{};
    desc.layout = pipeline_layout_;
    desc.vertexShader = vertex_shader_;
    desc.pixelShader = fragment_shader_;
    desc.renderTargetFormats[0] = rhi::DataFormat::RGBA16_Float;
    desc.renderTargetCount = 1;
    desc.depthStencilFormat = rhi::DataFormat::D32_Float;
    desc.enableDepthTest = true;    // 光斑被几何遮挡（深度只读）
    desc.enableDepthWrite = false;
    desc.depthFunc = rhi::ComparisonFunc::Less;
    desc.enableBlend = true;        // 加色：HDR 光贡献
    desc.srcColorBlendFactor = rhi::BlendFactor::One;
    desc.dstColorBlendFactor = rhi::BlendFactor::One;
    desc.colorBlendOp = rhi::BlendOp::Add;
    desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
    desc.dstAlphaBlendFactor = rhi::BlendFactor::One;
    desc.alphaBlendOp = rhi::BlendOp::Add;
    desc.cullMode = rhi::CullMode::None;
    desc.vertexAttributes.clear();
    desc.vertexBindings.clear();
    pipeline_ = device->CreateGraphicsPipeline(desc);
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "light pass: pipeline creation failed" << std::endl;
        return false;
    }

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetDesc set_desc{};
        set_desc.layout = descriptor_layout_;
        descriptor_sets_[i] = device->CreateDescriptorSet(set_desc);
    }

    rhi::BufferDesc ldesc{};
    ldesc.size = (u64)max_lights() * instance_stride();
    ldesc.type = rhi::BufferType::Structured;
    ldesc.usage = rhi::GPUMemoryUsage::Dynamic;
    ldesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    ldesc.structured.elementCount = max_lights();
    ldesc.structured.elementStride = instance_stride();
    rhi::BufferDesc udesc{};
    udesc.size = sizeof(VPUniforms);
    udesc.type = rhi::BufferType::Constant;
    udesc.usage = rhi::GPUMemoryUsage::Dynamic;
    udesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    udesc.structured.elementCount = 1;
    udesc.structured.elementStride = sizeof(VPUniforms);

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        light_buffers_[i] = device->CreateBuffer(ldesc);
        uniform_buffers_[i] = device->CreateBuffer(udesc);
        if (light_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            uniform_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "ParticleFX") << "light pass: buffer creation failed" << std::endl;
            return false;
        }
    }

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (descriptor_sets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) continue;
        rhi::WriteDescriptorSet writes[2];
        rhi::DescriptorBufferInfo infos[2];
        writes[0].dstSet = descriptor_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = rhi::DescriptorType::StorageBuffer;
        infos[0].buffer = light_buffers_[i];
        infos[0].offset = 0;
        infos[0].range = (u64)max_lights() * instance_stride();
        writes[0].bufferInfo = &infos[0];
        writes[1].dstSet = descriptor_sets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = rhi::DescriptorType::UniformBuffer;
        infos[1].buffer = uniform_buffers_[i];
        infos[1].offset = 0;
        infos[1].range = sizeof(VPUniforms);
        writes[1].bufferInfo = &infos[1];
        device->UpdateDescriptorSets(2, writes);
    }

    PRIMAL_LOG_STREAM(Info, "ParticleFX") << "light pass initialized (max_lights="
                                          << max_lights() << ")" << std::endl;
    return true;
}

void ParticleFXLightPass::shutdown() {
    device_ = nullptr;
    pipeline_ = rhi::handles::INVALID_PIPELINE;
    vertex_shader_ = fragment_shader_ = rhi::handles::INVALID_SHADER;
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        light_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        uniform_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        descriptor_sets_[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    staged_count_ = 0;
}

void ParticleFXLightPass::stage_lights(const void* data, u32 count, u32 frame_index) {
    staged_count_ = 0;
    if (!device_ || !data || count == 0 || count > max_lights()) return;
    const u32 frame = frame_index % rhi::MAX_FRAMES_IN_FLIGHT;
    void* mapped = device_->MapBuffer(light_buffers_[frame]);
    if (!mapped) return;
    std::memcpy(mapped, data, (size_t)count * instance_stride());
    device_->UnmapBuffer(light_buffers_[frame]);
    staged_count_ = count;
}

void ParticleFXLightPass::execute(rhi::RHICommandBuffer* cmd_buffer,
                                  u32 frame_index,
                                  const math::m4x4& view_matrix,
                                  const math::m4x4& projection_matrix) {
    if (!device_ || !cmd_buffer || staged_count_ == 0) return;
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) return;
    const u32 frame = frame_index % rhi::MAX_FRAMES_IN_FLIGHT;

    VPUniforms uni{};
    uni.view_projection = projection_matrix * view_matrix;
    if (void* mapped = device_->MapBuffer(uniform_buffers_[frame])) {
        std::memcpy(mapped, &uni, sizeof(uni));
        device_->UnmapBuffer(uniform_buffers_[frame]);
    }

    cmd_buffer->BindGraphicsPipeline(pipeline_);
    if (descriptor_sets_[frame] != rhi::handles::INVALID_DESCRIPTOR_SET) {
        cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipeline_layout_, 0, 1,
                                       &descriptor_sets_[frame], 0, nullptr);
    }
    cmd_buffer->Draw(6, 0, staged_count_, 0);
    if (!first_draw_logged_) {
        first_draw_logged_ = true;
        PRIMAL_LOG_STREAM(Info, "ParticleFX")
            << "[ParticleFXLightPass] first draw: " << staged_count_ << " lights" << std::endl;
    }
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
