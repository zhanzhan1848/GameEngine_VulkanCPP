// ParticleFXMeshPass.cpp — 见 ParticleFXMeshPass.h。

#include "ParticleFXMeshPass.h"
#include "ParticleFX/PfxTypes.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cmath>
#include <fstream>
#include <iterator>

namespace primal::graphics {

namespace {

constexpr u32 kMaxInstances = ParticleFXMeshPass::max_instances();
constexpr u32 kInstanceStride = 48;   // render::mesh_instance_data（Bridges.h 布局锁定）

// 程序化立方体（24 顶点带面法线，36 索引）
struct cube_mesh {
    math::v3 positions[24];
    math::v3 normals[24];
    u16 indices[36];
};

cube_mesh make_cube(float half) {
    cube_mesh m;
    const math::v3 px{ 1, 0, 0 }, nx{ -1, 0, 0 }, py{ 0, 1, 0 }, ny{ 0, -1, 0 }, pz{ 0, 0, 1 },
        nz{ 0, 0, -1 };
    const math::v3 nrm[6] = { px, nx, py, ny, pz, nz };
    // 每面 4 顶点（面内坐标基）
    const math::v3 uv[6][2] = { { pz, ny }, { nz, ny }, { px, nz }, { px, pz }, { nx, ny }, { nx, ny } };
    u32 v = 0;
    for (u32 f = 0; f < 6; ++f) {
        const math::v3 c = nrm[f] * half;
        const math::v3 u = uv[f][0] * half;
        const math::v3 w = uv[f][1] * half;
        const math::v3 corner[4] = { c - u - w, c + u - w, c + u + w, c - u + w };
        for (u32 i = 0; i < 4; ++i) {
            m.positions[v + i] = corner[i];
            m.normals[v + i] = nrm[f];
        }
        // 两三角（面外逆时针）
        m.indices[f * 6 + 0] = (u16)(v + 0);
        m.indices[f * 6 + 1] = (u16)(v + 1);
        m.indices[f * 6 + 2] = (u16)(v + 2);
        m.indices[f * 6 + 3] = (u16)(v + 0);
        m.indices[f * 6 + 4] = (u16)(v + 2);
        m.indices[f * 6 + 5] = (u16)(v + 3);
        v += 4;
    }
    (void)px;
    (void)nx;
    (void)py;
    (void)ny;
    (void)pz;
    (void)nz;
    return m;
}

std::string load_shader_source() {
    const char* candidates[] = {
        "EngineTest/shaders/ParticleFXMesh.metal",
        "shaders/ParticleFXMesh.metal",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/ParticleFXMesh.metal",
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

bool ParticleFXMeshPass::initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    if (device_) return true;   // 幂等

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
        std::vector<u8> vsb = loadSpvBinary("Engine/Graphics/Vulkan/Shaders/PFX/ParticleFXMesh.vert.spv");
        std::vector<u8> fsb = loadSpvBinary("Engine/Graphics/Vulkan/Shaders/PFX/ParticleFXMesh.frag.spv");
        if (vsb.empty() || fsb.empty()) {
            PRIMAL_LOG_STREAM(Info, "ParticleFX") << "mesh pass: SPIR-V not found, pass disabled" << std::endl;
            return true;
        }
        vertex_shader_ = device->CreateShader(vsb.data(), vsb.size(), rhi::ShaderStage::Vertex, "main");
        fragment_shader_ = device->CreateShader(fsb.data(), fsb.size(), rhi::ShaderStage::Pixel, "main");
        if (vertex_shader_ == rhi::handles::INVALID_SHADER || fragment_shader_ == rhi::handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: Vulkan shader creation failed" << std::endl;
            return true;
        }
    } else {


    // ---- shader（运行时加载 .metal 文本，沿 ParticlePass Metal 路径惯例）----
    const std::string code = load_shader_source();
    if (code.empty()) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: shader source not found" << std::endl;
        return false;
    }
    vertex_shader_ = device->CreateShader(code.c_str(), code.size(), rhi::ShaderStage::Vertex,
                                          "pfx_mesh_vertex");
    fragment_shader_ = device->CreateShader(code.c_str(), code.size(), rhi::ShaderStage::Pixel,
                                            "pfx_mesh_fragment");
    if (vertex_shader_ == rhi::handles::INVALID_SHADER ||
        fragment_shader_ == rhi::handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: shader creation failed" << std::endl;
        return false;
    }

    } // Metal shader 路径

    // ---- 描述符布局：0 positions / 1 normals / 2 instances / 3 uniforms ----
    rhi::DescriptorSetLayoutBinding bindings[] = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },   // positions
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },   // normals
        { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },   // instances
        { 3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Vertex },   // VP + color
    };
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = bindings;
    layout_desc.bindingCount = 4;
    descriptor_layout_ = device->CreateDescriptorSetLayout(layout_desc);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.setLayoutCount = 1;
    pl_desc.setLayouts = &descriptor_layout_;
    pipeline_layout_ = device->CreatePipelineLayout(pl_desc);

    // ---- 管线：不透明、深度测试只读、顶点拉取（无顶点属性）----
    rhi::GraphicsPipelineDesc desc{};
    desc.layout = pipeline_layout_;
    desc.vertexShader = vertex_shader_;
    desc.pixelShader = fragment_shader_;
    desc.renderTargetFormats[0] = rhi::DataFormat::RGBA16_Float;
    desc.renderTargetCount = 1;
    desc.depthStencilFormat = rhi::DataFormat::D32_Float;
    desc.enableDepthTest = true;
    desc.enableDepthWrite = false;
    desc.depthFunc = rhi::ComparisonFunc::Less;
    desc.enableBlend = false;
    desc.cullMode = rhi::CullMode::None;   // 实例旋转任意，v1 双面
    desc.vertexAttributes.clear();
    desc.vertexBindings.clear();
    pipeline_ = device->CreateGraphicsPipeline(desc);
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: pipeline creation failed" << std::endl;
        return false;
    }

    // ---- 描述符集 ----
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetDesc set_desc{};
        set_desc.layout = descriptor_layout_;
        descriptor_sets_[i] = device->CreateDescriptorSet(set_desc);
    }

    // ---- 网格缓冲（程序化立方体）----
    const cube_mesh cube = make_cube(0.05f);
    index_count_ = 36;

    rhi::BufferDesc vdesc{};
    vdesc.size = sizeof(cube.positions);
    vdesc.type = rhi::BufferType::Structured;
    vdesc.usage = rhi::GPUMemoryUsage::Dynamic;
    vdesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    vdesc.structured.elementCount = 24;
    vdesc.structured.elementStride = sizeof(math::v3);
    position_buffer_ = device->CreateBuffer(vdesc);
    normal_buffer_ = device->CreateBuffer(vdesc);
    rhi::BufferDesc idesc{};
    idesc.size = sizeof(cube.indices);
    idesc.type = rhi::BufferType::Structured;
    idesc.usage = rhi::GPUMemoryUsage::Dynamic;
    idesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    idesc.structured.elementCount = 36;
    idesc.structured.elementStride = sizeof(u16);
    index_buffer_ = device->CreateBuffer(idesc);
    if (position_buffer_ == rhi::handles::INVALID_RESOURCE ||
        normal_buffer_ == rhi::handles::INVALID_RESOURCE ||
        index_buffer_ == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: mesh buffer creation failed" << std::endl;
        return false;
    }
    {
        void* mp = device->MapBuffer(position_buffer_);
        if (mp) std::memcpy(mp, cube.positions, sizeof(cube.positions));
        device->UnmapBuffer(position_buffer_);
        mp = device->MapBuffer(normal_buffer_);
        if (mp) std::memcpy(mp, cube.normals, sizeof(cube.normals));
        device->UnmapBuffer(normal_buffer_);
        mp = device->MapBuffer(index_buffer_);
        if (mp) std::memcpy(mp, cube.indices, sizeof(cube.indices));
        device->UnmapBuffer(index_buffer_);
    }

    // ---- per-frame 实例 / 常量缓冲 ----
    rhi::BufferDesc inst_desc{};
    inst_desc.size = (u64)kMaxInstances * kInstanceStride;
    inst_desc.type = rhi::BufferType::Structured;
    inst_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    inst_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    inst_desc.structured.elementCount = kMaxInstances;
    inst_desc.structured.elementStride = kInstanceStride;
    rhi::BufferDesc uni_desc{};
    uni_desc.size = sizeof(VPUniforms);
    uni_desc.type = rhi::BufferType::Structured;
    uni_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    uni_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    uni_desc.structured.elementCount = 1;
    uni_desc.structured.elementStride = sizeof(VPUniforms);
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        instance_buffers_[i] = device->CreateBuffer(inst_desc);
        uniform_buffers_[i] = device->CreateBuffer(uni_desc);
        if (instance_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            uniform_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: frame buffer creation failed" << std::endl;
            return false;
        }
    }

    // ---- 描述集写入 ----
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (descriptor_sets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) continue;
        rhi::WriteDescriptorSet writes[4];
        rhi::DescriptorBufferInfo infos[4];
        for (u32 b = 0; b < 4; ++b) {
            writes[b].dstSet = descriptor_sets_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 3 ? rhi::DescriptorType::UniformBuffer
                                              : rhi::DescriptorType::StorageBuffer;
        }
        infos[0].buffer = position_buffer_;
        infos[0].offset = 0;
        infos[0].range = sizeof(cube.positions);
        writes[0].bufferInfo = &infos[0];
        infos[1].buffer = normal_buffer_;
        infos[1].offset = 0;
        infos[1].range = sizeof(cube.positions);
        writes[1].bufferInfo = &infos[1];
        infos[2].buffer = instance_buffers_[i];
        infos[2].offset = 0;
        infos[2].range = (u64)kMaxInstances * kInstanceStride;
        writes[2].bufferInfo = &infos[2];
        infos[3].buffer = uniform_buffers_[i];
        infos[3].offset = 0;
        infos[3].range = sizeof(VPUniforms);
        writes[3].bufferInfo = &infos[3];
        device->UpdateDescriptorSets(4, writes);
    }

    PRIMAL_LOG_STREAM(Info, "ParticleFX") << "mesh pass initialized (max_instances=" << kMaxInstances
                                          << ")" << std::endl;
    return true;
}

void ParticleFXMeshPass::shutdown() {
    device_ = nullptr;
    pipeline_ = rhi::handles::INVALID_PIPELINE;
    vertex_shader_ = fragment_shader_ = rhi::handles::INVALID_SHADER;
    position_buffer_ = normal_buffer_ = index_buffer_ = rhi::handles::INVALID_RESOURCE;
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        instance_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        uniform_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        descriptor_sets_[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    staged_count_ = 0;
}

void ParticleFXMeshPass::stage_instances(const void* data, u32 count, u32 frame_index) {
    staged_count_ = 0;
    if (!device_ || !data || count == 0 || count > max_instances()) return;
    const u32 frame = frame_index % rhi::MAX_FRAMES_IN_FLIGHT;
    void* mapped = device_->MapBuffer(instance_buffers_[frame]);
    if (!mapped) return;
    std::memcpy(mapped, data, (size_t)count * kInstanceStride);
    device_->UnmapBuffer(instance_buffers_[frame]);
    staged_count_ = count;
}

void ParticleFXMeshPass::execute(rhi::RHICommandBuffer* cmd_buffer,
                                 u32 frame_index,
                                 const math::m4x4& view_matrix,
                                 const math::m4x4& projection_matrix) {
    if (!device_ || !cmd_buffer || staged_count_ == 0) return;
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) return;   // Vulkan 等未实现后端早退
    const u32 frame = frame_index % rhi::MAX_FRAMES_IN_FLIGHT;

    // 常量更新
    VPUniforms uni{};
    uni.view_projection = projection_matrix * view_matrix;
    uni.color = math::v4{ color_[0], color_[1], color_[2], color_[3] };
    if (void* mapped = device_->MapBuffer(uniform_buffers_[frame])) {
        std::memcpy(mapped, &uni, sizeof(uni));
        device_->UnmapBuffer(uniform_buffers_[frame]);
    }

    cmd_buffer->BindGraphicsPipeline(pipeline_);
    if (descriptor_sets_[frame] != rhi::handles::INVALID_DESCRIPTOR_SET) {
        cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipeline_layout_, 0, 1,
                                       &descriptor_sets_[frame], 0, nullptr);
    }
    cmd_buffer->BindIndexBuffer(index_buffer_, rhi::DataFormat::R16_UInt, 0);
    // 索引化实例绘制：36 索引 × staged_count 实例
    cmd_buffer->DrawIndexed(index_count_, 0, 0, staged_count_, 0);
    if (!first_draw_logged_) {
        first_draw_logged_ = true;
        PRIMAL_LOG_STREAM(Info, "ParticleFX")
            << "[ParticleFXMeshPass] first draw: " << staged_count_ << " instances x "
            << index_count_ << " indices" << std::endl;
    }
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
