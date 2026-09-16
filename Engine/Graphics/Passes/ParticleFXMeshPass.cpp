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

// 程序化 UV 球（内置几何槽 1）。positions 与 normals 同形（单位球向径）。
struct sphere_mesh {
    std::vector<math::v3> positions;
    std::vector<math::v3> normals;
    std::vector<u32> indices;
};

sphere_mesh make_uv_sphere(float radius, u32 stacks, u32 slices) {
    constexpr f32 kPi = 3.14159265f;
    sphere_mesh m;
    if (stacks < 2 || slices < 3) return m;
    m.positions.reserve((u64)(stacks + 1) * slices);
    m.normals.reserve(m.positions.capacity());
    for (u32 h = 0; h <= stacks; ++h) {
        const f32 theta = kPi * (f32)h / (f32)stacks;
        for (u32 w = 0; w < slices; ++w) {
            const f32 phi = 2.f * kPi * (f32)w / (f32)slices;
            math::v3 n{ std::sin(theta) * std::cos(phi), std::cos(theta),
                        std::sin(theta) * std::sin(phi) };
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
        }
    }
    auto at = [slices](u32 h, u32 w) { return h * slices + (w % slices); };
    for (u32 h = 0; h + 1 < stacks; ++h) {
        for (u32 w = 0; w < slices; ++w) {
            const u32 a = at(h, w), b = at(h, w + 1), c = at(h + 1, w), d = at(h + 1, w + 1);
            if (h > 0) {   // 上三角（极点行退化跳过）
                m.indices.push_back(a);
                m.indices.push_back(c);
                m.indices.push_back(b);
            }
            if (h + 2 < stacks) {   // 下三角
                m.indices.push_back(b);
                m.indices.push_back(c);
                m.indices.push_back(d);
            }
        }
    }
    return m;
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

    // ---- 几何槽 0：内置程序化立方体 ----
    const cube_mesh cube = make_cube(0.05f);
    auto& cube_slot = slots_[0];
    cube_slot.vertex_count = 24;
    cube_slot.index_count = 36;
    cube_slot.index_u32 = false;   // 立方体保持 R16 索引
    if (!create_buffer_and_upload(cube_slot.positions, sizeof(cube.positions), 24, sizeof(math::v3),
                                  cube.positions) ||
        !create_buffer_and_upload(cube_slot.normals, sizeof(cube.normals), 24, sizeof(math::v3),
                                  cube.normals) ||
        !create_buffer_and_upload(cube_slot.index, sizeof(cube.indices), 36, sizeof(u16),
                                  cube.indices)) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: mesh buffer creation failed" << std::endl;
        return false;
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
    uni_desc.type = rhi::BufferType::Constant;
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

    // ---- 槽 0 描述符集 ----
    if (!create_geometry_descriptor_sets(cube_slot)) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: descriptor set creation failed" << std::endl;
        return false;
    }

    // ---- 几何槽 1：内置程序化球体（builtin:sphere）----
    const sphere_mesh sphere = make_uv_sphere(0.05f, 12, 18);
    auto& sphere_slot = slots_[ParticleFXMeshPass::builtin_sphere_slot()];
    sphere_slot.vertex_count = (u32)sphere.positions.size();
    sphere_slot.index_count = (u32)sphere.indices.size();
    sphere_slot.index_u32 = true;
    if (sphere_slot.vertex_count > 0 &&
        create_buffer_and_upload(sphere_slot.positions,
                                 (u64)sphere.positions.size() * sizeof(math::v3),
                                 (u32)sphere.positions.size(), sizeof(math::v3),
                                 sphere.positions.data()) &&
        create_buffer_and_upload(sphere_slot.normals,
                                 (u64)sphere.normals.size() * sizeof(math::v3),
                                 (u32)sphere.normals.size(), sizeof(math::v3),
                                 sphere.normals.data()) &&
        create_buffer_and_upload(sphere_slot.index,
                                 (u64)sphere.indices.size() * sizeof(u32),
                                 (u32)sphere.indices.size(), sizeof(u32),
                                 sphere.indices.data()) &&
        create_geometry_descriptor_sets(sphere_slot)) {
        geometry_count_ = 2;   // 槽 0/1 均为内置；外部几何从槽 2 起
        PRIMAL_LOG_STREAM(Info, "ParticleFX")
            << "mesh pass: builtin sphere registered (slot 1, " << sphere_slot.vertex_count
            << " verts)" << std::endl;
    } else {
        PRIMAL_LOG_STREAM(Warn, "ParticleFX")
            << "mesh pass: builtin sphere creation failed — builtin:sphere falls back to cube"
            << std::endl;
    }

    PRIMAL_LOG_STREAM(Info, "ParticleFX") << "mesh pass initialized (max_instances=" << kMaxInstances
                                          << ")" << std::endl;
    return true;
}

bool ParticleFXMeshPass::create_buffer_and_upload(rhi::ResourceHandle& out, u64 size,
                                                  u32 element_count, u32 element_stride,
                                                  const void* data) {
    rhi::BufferDesc desc{};
    desc.size = size;
    desc.type = rhi::BufferType::Structured;
    desc.usage = rhi::GPUMemoryUsage::Dynamic;
    desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    desc.structured.elementCount = element_count;
    desc.structured.elementStride = element_stride;
    out = device_->CreateBuffer(desc);
    if (out == rhi::handles::INVALID_RESOURCE) return false;
    if (void* mapped = device_->MapBuffer(out)) {
        std::memcpy(mapped, data, (size_t)size);
        device_->UnmapBuffer(out);
        return true;
    }
    return false;
}

bool ParticleFXMeshPass::create_geometry_descriptor_sets(geometry_buffers& g) {
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetDesc set_desc{};
        set_desc.layout = descriptor_layout_;
        g.descriptor_sets[i] = device_->CreateDescriptorSet(set_desc);
        if (g.descriptor_sets[i] == rhi::handles::INVALID_DESCRIPTOR_SET) return false;
        rhi::WriteDescriptorSet writes[4];
        rhi::DescriptorBufferInfo infos[4];
        for (u32 b = 0; b < 4; ++b) {
            writes[b].dstSet = g.descriptor_sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 3 ? rhi::DescriptorType::UniformBuffer
                                              : rhi::DescriptorType::StorageBuffer;
        }
        const u64 vtx_range = (u64)g.vertex_count * sizeof(math::v3);
        infos[0].buffer = g.positions;
        infos[0].offset = 0;
        infos[0].range = vtx_range;
        writes[0].bufferInfo = &infos[0];
        infos[1].buffer = g.normals;
        infos[1].offset = 0;
        infos[1].range = vtx_range;
        writes[1].bufferInfo = &infos[1];
        infos[2].buffer = instance_buffers_[i];
        infos[2].offset = 0;
        infos[2].range = (u64)kMaxInstances * kInstanceStride;
        writes[2].bufferInfo = &infos[2];
        infos[3].buffer = uniform_buffers_[i];
        infos[3].offset = 0;
        infos[3].range = sizeof(VPUniforms);
        writes[3].bufferInfo = &infos[3];
        device_->UpdateDescriptorSets(4, writes);
    }
    return true;
}

u32 ParticleFXMeshPass::register_geometry(const math::v3* positions, const math::v3* normals,
                                          u32 vertex_count, const u32* indices, u32 index_count) {
    if (!device_ || !positions || !normals || !indices || vertex_count == 0 || index_count == 0)
        return u32_invalid_id;
    if (geometry_count_ >= max_geometry_slots()) {
        PRIMAL_LOG_STREAM(Warn, "ParticleFX")
            << "mesh pass: geometry slots full (" << max_geometry_slots() << "), using cube fallback"
            << std::endl;
        return u32_invalid_id;
    }
    geometry_buffers& g = slots_[geometry_count_];
    g.vertex_count = vertex_count;
    g.index_count = index_count;
    g.index_u32 = true;   // 外部几何统一 u32 索引（load_instance_geometry 已扩展）
    if (!create_buffer_and_upload(g.positions, (u64)vertex_count * sizeof(math::v3), vertex_count,
                                  sizeof(math::v3), positions) ||
        !create_buffer_and_upload(g.normals, (u64)vertex_count * sizeof(math::v3), vertex_count,
                                  sizeof(math::v3), normals) ||
        !create_buffer_and_upload(g.index, (u64)index_count * sizeof(u32), index_count, sizeof(u32),
                                  indices)) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "mesh pass: instance geometry buffer creation failed"
                                               << std::endl;
        return u32_invalid_id;
    }
    if (!create_geometry_descriptor_sets(g)) {
        PRIMAL_LOG_STREAM(Error, "ParticleFX")
            << "mesh pass: instance geometry descriptor creation failed" << std::endl;
        return u32_invalid_id;
    }
    PRIMAL_LOG_STREAM(Info, "ParticleFX") << "mesh pass: registered instance geometry slot "
                                          << geometry_count_ << " (" << vertex_count << " verts, "
                                          << index_count << " indices)" << std::endl;
    return geometry_count_++;
}

void ParticleFXMeshPass::shutdown() {
    device_ = nullptr;
    pipeline_ = rhi::handles::INVALID_PIPELINE;
    vertex_shader_ = fragment_shader_ = rhi::handles::INVALID_SHADER;
    for (u32 s = 0; s < max_geometry_slots(); ++s) {
        geometry_buffers& g = slots_[s];
        g.positions = g.normals = g.index = rhi::handles::INVALID_RESOURCE;
        g.vertex_count = g.index_count = 0;
        for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i)
            g.descriptor_sets[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    geometry_count_ = 1;
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        instance_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        uniform_buffers_[i] = rhi::handles::INVALID_RESOURCE;
    }
    descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    staged_count_ = 0;
}

void ParticleFXMeshPass::stage_instances(const void* data, const u32* instance_slots, u32 count,
                                         u32 frame_index) {
    staged_count_ = 0;
    if (!device_ || !data || count == 0 || count > max_instances()) return;
    const u32 frame = frame_index % rhi::MAX_FRAMES_IN_FLIGHT;
    void* mapped = device_->MapBuffer(instance_buffers_[frame]);
    if (!mapped) return;
    std::memcpy(mapped, data, (size_t)count * kInstanceStride);
    device_->UnmapBuffer(instance_buffers_[frame]);
    for (u32 i = 0; i < count; ++i)
        instance_slots_[i] = (instance_slots && instance_slots[i] < geometry_count_)
                                 ? instance_slots[i]
                                 : 0u;
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
    // 按几何槽分组绘制：实例按发射器序打包，同槽实例天然连续，扫描即得分组。
    u32 start = 0;
    while (start < staged_count_) {
        const geometry_buffers& g = slot_geometry(instance_slots_[start]);
        u32 end = start + 1;
        while (end < staged_count_ &&
               &slot_geometry(instance_slots_[end]) == &g)
            ++end;
        if (g.descriptor_sets[frame] != rhi::handles::INVALID_DESCRIPTOR_SET) {
            cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipeline_layout_, 0, 1,
                                           &g.descriptor_sets[frame], 0, nullptr);
        }
        cmd_buffer->BindIndexBuffer(g.index,
                                    g.index_u32 ? rhi::DataFormat::R32_UInt : rhi::DataFormat::R16_UInt,
                                    0);
        cmd_buffer->DrawIndexed(g.index_count, 0, 0, end - start, start);
        start = end;
    }
    if (!first_draw_logged_) {
        first_draw_logged_ = true;
        PRIMAL_LOG_STREAM(Info, "ParticleFX")
            << "[ParticleFXMeshPass] first draw: " << staged_count_ << " instances in "
            << (instance_slots_[0] ? "instance" : "cube") << " geometry" << std::endl;
    }
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
