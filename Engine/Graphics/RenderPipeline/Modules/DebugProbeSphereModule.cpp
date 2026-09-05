#include "DebugProbeSphereModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;
using namespace rendergraph;

namespace {
// B1 L3: 自加载着色器字节码（GPUDrivenDrawPipeline::LoadShaderBytecode 同款
// 双候选路径风格）。Metal 读 MSL 源（device 运行时编译）；Vulkan 读 Debug/
// ProbeSphere.{vert,frag}.spv（精确字节数）。其它平台返回空——probe 球
// overlay 在无填充后端上禁用。
std::vector<u8> LoadProbeSphereBytecode(rhi::RHIDeviceBase* device, const char* name) {
    const auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;
    std::vector<std::string> candidates;
    if (platform == rhi::RHIPlatform::Vulkan) {
        std::string file = std::string(name) + ".spv";
        candidates.push_back("Engine/Graphics/Vulkan/shaders/Debug/" + file);
        candidates.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Vulkan/shaders/Debug/" + file);
    } else if (platform == rhi::RHIPlatform::Metal) {
        // MSL 是单文件双入口（probe_sphere_vs/_fs）：剥掉 .vert/.frag 后缀。
        std::string stem(name);
        for (const char* suffix : {".vert", ".frag"}) {
            auto pos = stem.rfind(suffix);
            if (pos != std::string::npos && pos + std::strlen(suffix) == stem.size())
                stem.erase(pos);
        }
        std::string file = stem + ".metal";
        candidates.push_back("EngineTest/shaders/" + file);
        candidates.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/" + file);
    } else {
        return {};
    }

    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) continue;
        if (platform == rhi::RHIPlatform::Metal) return bytes;           // MSL 源
        if (size % 4 != 0) continue;                                      // SPIR-V 4B 对齐
        return bytes;
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] shader bytecode not found for '"
              << name << "' (tried " << candidates.size() << " paths)" << std::endl;
    return {};
}
} // namespace

namespace {
// Unit icosphere: 12 verts, 20 faces; subdivided 1-to-4 on the CPU (42 verts,
// 80 tris) so shading is smooth enough to read at hallway distances.
void BuildIcosphere(std::vector<float>& vb, std::vector<uint16_t>& ib) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    float raw[12][3] = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1},
    };
    for (auto& v : raw) {
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        vb.push_back(v[0] / len); vb.push_back(v[1] / len); vb.push_back(v[2] / len);
    }
    uint16_t f[60] = {
        0,1,4,   0,4,9,   9,4,5,   4,8,5,   4,1,8,
        8,1,10,  8,10,3,  5,8,3,   5,3,2,   2,3,7,
        3,10,6,  3,6,7,   6,10,1,  6,1,0,   0,9,6,
        6,9,11,  11,9,5,  5,11,2,  11,7,2,  1,10,11
    };
    // 1-to-4 subdivision with midpoint projection.
    auto mid = [&](uint16_t a, uint16_t b) {
        float ax = vb[a * 3], ay = vb[a * 3 + 1], az = vb[a * 3 + 2];
        float bx = vb[b * 3], by = vb[b * 3 + 1], bz = vb[b * 3 + 2];
        float mx = ax + bx, my = ay + by, mz = az + bz;
        float len = std::sqrt(mx * mx + my * my + mz * mz);
        vb.push_back(mx / len); vb.push_back(my / len); vb.push_back(mz / len);
        return static_cast<uint16_t>(vb.size() / 3 - 1);
    };
    for (int i = 0; i < 20; ++i) {
        uint16_t a = f[i * 3], b = f[i * 3 + 1], c = f[i * 3 + 2];
        uint16_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
        uint16_t sub[4][3] = { {a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca} };
        for (auto& tri : sub) {
            ib.push_back(tri[0]); ib.push_back(tri[1]); ib.push_back(tri[2]);
        }
    }
}
} // namespace

bool DebugProbeSphereModule::Initialize(RHIDeviceBase* device,
                                        ShaderHandle vertex_shader,
                                        ShaderHandle pixel_shader) {
    device_ = device;
    // B1 L3: 句柄未注入时自加载（编辑器路径无 SetShaderHandles 引导——
    // 生产管线逐模块自加载字节码，本模块对齐该模式）。
    if (vertex_shader == handles::INVALID_SHADER) {
        // Metal: single MSL file both entries ("ProbeSphere.metal").
        // Vulkan: keep .vert/.frag two files (bytecode is pre-built).
        const char* vsName = (device->GetPlatform() == rhi::RHIPlatform::Metal)
                                 ? "ProbeSphere" : "ProbeSphere.vert";
        auto vs = LoadProbeSphereBytecode(device, vsName);
        if (!vs.empty())
            vertex_shader = device_->CreateShader(vs.data(), vs.size(),
                                                  ShaderStage::Vertex, "probe_sphere_vs");
    }
    ShaderHandle irradiance_ps = handles::INVALID_SHADER;
    if (pixel_shader == handles::INVALID_SHADER) {
        const bool metal = device->GetPlatform() == rhi::RHIPlatform::Metal;
        const char* psName = metal ? "ProbeSphere" : "ProbeSphere.frag";
        auto ps = LoadProbeSphereBytecode(device, psName);
        if (!ps.empty())
            pixel_shader = device_->CreateShader(ps.data(), ps.size(),
                                                 ShaderStage::Pixel, "probe_sphere_fs");
        if (metal) {
            auto psIrr = LoadProbeSphereBytecode(device, psName);
            if (!psIrr.empty())
                irradiance_ps = device_->CreateShader(psIrr.data(), psIrr.size(),
                                                      ShaderStage::Pixel, "probe_sphere_irradiance_fs");
        } else if (device->GetPlatform() == rhi::RHIPlatform::Vulkan) {
            // B1 L3.2: Vulkan 用独立 spv（ProbeSphereIrradiance.frag.spv）。
            auto psIrr = LoadProbeSphereBytecode(device, "ProbeSphereIrradiance.frag");
            if (!psIrr.empty())
                irradiance_ps = device_->CreateShader(psIrr.data(), psIrr.size(),
                                                      ShaderStage::Pixel, "main");
        }
    }
    if (vertex_shader == handles::INVALID_SHADER ||
        pixel_shader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] shaders unavailable — debug viz disabled" << std::endl;
        return false;
    }

    // DIAGNOSTIC: mesh16 (the only rendering variant) used a DESCRIPTOR-set
    // layout — mirror that shape with a single UBO binding (VS), unused by the
    // current diagnostic VS.
    DescriptorSetLayoutBinding ubo_binding{};
    ubo_binding.binding = 0;
    ubo_binding.descriptorType = DescriptorType::UniformBuffer;
    ubo_binding.descriptorCount = 1;
    ubo_binding.stageFlags = ShaderStage::Vertex;
    // B1 L3.2: FS storage buffer carrying the SH9 irradiance coefficients.
    DescriptorSetLayoutBinding irr_binding{};
    irr_binding.binding = 1;
    irr_binding.descriptorType = DescriptorType::StorageBuffer;
    irr_binding.descriptorCount = 1;
    irr_binding.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutBinding layoutBindings[2] = { ubo_binding, irr_binding };
    DescriptorSetLayoutDesc ds_desc{};
    ds_desc.bindingCount = 2;
    ds_desc.bindings = layoutBindings;
    set_layout_ = device_->CreateDescriptorSetLayout(ds_desc);
    // Set 0 (UBO) + VS push-constant range (view_proj) — BOTH must be
    // declared or the shader's push block reads an unbacked zero matrix.
    PipelineLayoutDesc pl_desc{};
    PushConstantRange pc_range{};
    pc_range.stageFlags = ShaderStage::Vertex;
    pc_range.offset = 0;
    pc_range.size = 112;  // matches the VS push_constant block (mat4+2 vec4)
    pl_desc.pushConstantRangeCount = 1;
    pl_desc.pushConstantRanges = &pc_range;
    pl_desc.setLayoutCount = 1;
    pl_desc.setLayouts = &set_layout_;
    layout_ = device_->CreatePipelineLayout(pl_desc);
    for (int i = 0; i < 3; ++i)
        sets_[i] = device_->CreateDescriptorSet({set_layout_});
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 128;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
        ubo_[i] = device_->CreateBuffer(desc);
    }

    GraphicsPipelineDesc pd{};
    pd.vertexShader = vertex_shader;
    pd.pixelShader = pixel_shader;
    pd.layout = layout_;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.fillMode = FillMode::Solid;
    pd.cullMode = CullMode::None;
    pd.renderTargetCount = 1;
    pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pd.enableBlend = false;
    // Flat stream: pos(3f) + normal(3f) + probeIdx(1f) = 28B, one binding.
    VertexInputAttribute attrs[] = {
        {0, 0, DataFormat::RGB32_Float,  0},
        {1, 0, DataFormat::RGB32_Float, 12},
        {2, 0, DataFormat::R32_Float,   24},
    };
    VertexInputBinding binds[] = {
        {0, 28, true},
    };
    pd.vertexAttributes.assign(std::begin(attrs), std::end(attrs));
    pd.vertexBindings.assign(std::begin(binds), std::end(binds));

    // B1 L3 遮挡: depth-on 变体挂 GBuffer 深度附件（Load/Don'tCare 只读），
    // 球体被场景几何遮挡处深度测试失败；depth-off 变体与 L3 之前行为一致
    //（Vulkan 测试路径不传 depth_rg 时使用）。
    auto makePipeline = [&](ShaderHandle ps, bool depth) {
        GraphicsPipelineDesc d = pd;
        d.pixelShader = ps;
        if (depth) {
            d.depthStencilFormat = DataFormat::D32_Float;
            d.enableDepthTest = true;
            d.depthFunc = ComparisonFunc::Less;
            d.enableDepthWrite = false;
        } else {
            d.depthStencilFormat = DataFormat::Unknown;
            d.enableDepthTest = false;
            d.enableDepthWrite = false;
        }
        return device_->CreateGraphicsPipeline(d);
    };

    pipeline_ = makePipeline(pixel_shader, /*depth=*/true);
    pipeline_nodepth_ = makePipeline(pixel_shader, /*depth=*/false);
    if (irradiance_ps != handles::INVALID_SHADER) {
        irradiance_pipeline_ = makePipeline(irradiance_ps, /*depth=*/true);
        irradiance_nodepth_pipeline_ = makePipeline(irradiance_ps, /*depth=*/false);
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] initialized (pipeline=" << pipeline_
              << " nodepth=" << pipeline_nodepth_
              << " irradiance=" << irradiance_pipeline_
              << " irradiance_nodepth=" << irradiance_nodepth_pipeline_ << ")" << std::endl;
    return pipeline_ != handles::INVALID_PIPELINE;
}

void DebugProbeSphereModule::Shutdown() {
    if (device_ == nullptr) return;
    if (sphere_vb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(sphere_vb_);
    sphere_vb_ = handles::INVALID_RESOURCE;
}

// Flat expanded vertex stream: every probe's icosphere baked out explicitly
// (pos + normal + probeIdx per vertex). No instancing, no index buffer, no
// gl_InstanceIndex — a plain full-mesh draw, structurally identical to any
// scene mesh. 60 verts per probe (20-face icosphere, flat).
bool DebugProbeSphereModule::BuildMeshAndInstances(const math::v3& origin,
                                                   const u32 counts[3],
                                                   u32 count, f32 spacing,
                                                   f32 radius) {
    if (sphere_vb_ != handles::INVALID_RESOURCE &&
        instance_count_ == count * 60 &&
        instances_spacing_ == spacing && instances_radius_ == radius &&
        instances_origin_.x == origin.x && instances_origin_.y == origin.y &&
        instances_origin_.z == origin.z)
        return true;

    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    float raw[12][3] = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1},
    };
    for (auto& v : raw) {
        float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
    uint16_t f[60] = {
        0,1,4,   0,4,9,   9,4,5,   4,8,5,   4,1,8,
        8,1,10,  8,10,3,  5,8,3,   5,3,2,   2,3,7,
        3,10,6,  3,6,7,   6,10,1,  6,1,0,   0,9,6,
        6,9,11,  11,9,5,  5,11,2,  11,7,2,  1,10,11
    };

    std::vector<float> vb;
    vb.reserve((u64)count * 60 * 7);
    for (u32 z = 0; z < counts[2]; ++z)
    for (u32 y = 0; y < counts[1]; ++y)
    for (u32 x = 0; x < counts[0]; ++x) {
        const float cx = origin.x + (float)x * spacing;
        const float cy = origin.y + (float)y * spacing;
        const float cz = origin.z + (float)z * spacing;
        const float idx = (float)(x + y * counts[0] + z * counts[0] * counts[1]);
        for (int k = 0; k < 20; ++k) {
            const uint16_t v0 = f[k*3], v1 = f[k*3+1], v2 = f[k*3+2];
            float e1[3] = {raw[v1][0]-raw[v0][0], raw[v1][1]-raw[v0][1], raw[v1][2]-raw[v0][2]};
            float e2[3] = {raw[v2][0]-raw[v0][0], raw[v2][1]-raw[v0][1], raw[v2][2]-raw[v0][2]};
            float n[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
            float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (nl < 1e-8f) nl = 1.0f;
            for (int c = 0; c < 3; ++c) {
                const uint16_t vi = f[k*3+c];
                vb.push_back(raw[vi][0] * radius + cx);
                vb.push_back(raw[vi][1] * radius + cy);
                vb.push_back(raw[vi][2] * radius + cz);
                vb.push_back(n[0]/nl); vb.push_back(n[1]/nl); vb.push_back(n[2]/nl);
                vb.push_back(idx);
            }
        }
    }

    if (sphere_vb_ != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(sphere_vb_);
    BufferDesc vdesc{};
    vdesc.size = vb.size() * sizeof(float);
    vdesc.type = BufferType::Vertex;
    vdesc.usage = GPUMemoryUsage::Dynamic;   // Static = unmappable in this RHI
    vdesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vdesc.bindFlags = static_cast<u32>(BufferUsageFlags::Vertex);
    sphere_vb_ = device_->CreateBuffer(vdesc);
    if (auto* m = device_->MapBuffer(sphere_vb_)) {
        memcpy(m, vb.data(), vdesc.size);
        device_->UnmapBuffer(sphere_vb_);
    }
    instance_count_ = count * 60;
    instances_origin_ = origin;
    instances_spacing_ = spacing;
    instances_radius_ = radius;

    static bool logged = false;
    if (!logged) {
        logged = true;
        PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] expanded vb=" << sphere_vb_
                  << " verts=" << instance_count_
                  << " bytes=" << vdesc.size << std::endl;
    }
    return sphere_vb_ != handles::INVALID_RESOURCE;
}

void DebugProbeSphereModule::AddPass(RenderGraph& graph,
                                     const DebugProbeSphereInputs& inputs) {
    if (pipeline_ == handles::INVALID_PIPELINE || inputs.probe_count == 0) return;

    const f32 radius = inputs.sphere_radius > 0.0f ? inputs.sphere_radius
                                                   : inputs.probe_spacing * 0.15f;
    if (!BuildMeshAndInstances(inputs.probe_origin, inputs.probe_counts,
                               inputs.probe_count, inputs.probe_spacing, radius))
        return;
    const u32 cbIdx = inputs.current_buffer_index % 3;

    const bool occlusion = inputs.depth_rg.IsValid() &&
                           inputs.depth_width == inputs.render_width &&
                           inputs.depth_height == inputs.render_height;

    struct SphereData {
        RGResourceHandle output;
    };

    // 尺寸匹配才挂深度附件（render_scale != 1 时 GBuffer 深度与 overlay
    // 目标尺寸不一致 → 退回无遮挡 overlay）。
    graph.AddPass<SphereData>(
        "DebugProbeSpheres",
        RGPassType::Graphics,
        RGPassCategory::PostProcess,
        [inputs, occlusion](SphereData& data, RenderGraphBuilder& builder) {
            if (inputs.blit_output_rg.IsValid())
                builder.Read(inputs.blit_output_rg, ResourceState::RenderTarget);
            if (occlusion)
                builder.Read(inputs.depth_rg, ResourceState::DepthStencilReadOnly);
            data.output = builder.Write(inputs.backbuffer_rg,
                                        ResourceState::RenderTarget);
            RGRenderPassDesc rp;
            rp.colors.push_back({.texture = data.output,
                                 .loadOp = LoadAction::Load,  // overlay on blit result
                                 .storeOp = StoreAction::Store,
                                 .clearColor = {math::v4{0, 0, 0, 1}}});
            if (occlusion) {
                rp.depthStencil.texture = inputs.depth_rg;
                rp.depthStencil.loadOp = LoadAction::Load;          // 保留场景深度
                rp.depthStencil.storeOp = StoreAction::DontCare;    // 只读——不回写
                rp.depthStencil.depthLoadOp = LoadAction::Load;
                rp.depthStencil.depthStoreOp = StoreAction::DontCare;
            }
            builder.DeclareRenderPass(rp);
        },
        [this, inputs, cbIdx, occlusion](const SphereData&, RenderGraphContext& ctx) {
            static u32 execN = 0;
            if (++execN == 1u || execN % 60u == 0u)
                PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] execute #" << execN
                          << " instances=" << instance_count_ << std::endl;
            auto cmd = ctx.cmdBuffer;

            const bool useIrradiance =
                inputs.irradiance_buffer != handles::INVALID_RESOURCE &&
                irradiance_pipeline_ != handles::INVALID_PIPELINE;

            math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
            if (auto* u = static_cast<math::m4x4*>(device_->MapBuffer(ubo_[cbIdx]))) {
                *u = vp;
                device_->UnmapBuffer(ubo_[cbIdx]);
            }
            struct PCData {
                math::m4x4 view_proj;
                math::v4 sun_dir;
                math::v4 cam_pos;
            } pcData{vp, inputs.sun_dir, inputs.camera_position};
            DescriptorBufferInfo binfo{ubo_[cbIdx], 0, 128};
            WriteDescriptorSet w[2]{};
            u32 writeCount = 1;
            w[0].dstSet = sets_[cbIdx];
            w[0].dstBinding = 0;
            w[0].dstArrayElement = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = DescriptorType::UniformBuffer;
            w[0].bufferInfo = &binfo;
            // B1 L3.2: 只在 irradiance 缓冲有效时写 binding 1——空句柄写入
            // 会在 Vulkan validation 报 NULL storage buffer。
            if (inputs.irradiance_buffer != handles::INVALID_RESOURCE) {
                DescriptorBufferInfo irrInfo{inputs.irradiance_buffer, 0, ~0ULL};
                w[1].dstSet = sets_[cbIdx];
                w[1].dstBinding = 1;
                w[1].dstArrayElement = 0;
                w[1].descriptorCount = 1;
                w[1].descriptorType = DescriptorType::StorageBuffer;
                w[1].bufferInfo = &irrInfo;
                writeCount = 2;
            }
            device_->UpdateDescriptorSets(writeCount, w);

            cmd->SetViewport({{0, 0},
                              {(float)inputs.render_width, (float)inputs.render_height},
                              0, 1});
            cmd->SetScissor({{0, 0}, {inputs.render_width, inputs.render_height}});
            const rhi::PipelineHandle psPipeline = useIrradiance
                ? (occlusion ? irradiance_pipeline_ : irradiance_nodepth_pipeline_)
                : (occlusion ? pipeline_ : pipeline_nodepth_);
            cmd->BindGraphicsPipeline(psPipeline != handles::INVALID_PIPELINE
                                          ? psPipeline
                                          : pipeline_);
            cmd->PushConstants(layout_, ShaderStage::Vertex,
                               0, sizeof(PCData), &pcData);
            ResourceHandle vbs[1] = {sphere_vb_};
            u64 offsets[1] = {0};
            cmd->BindVertexBuffers(0, 1, vbs, offsets);
            cmd->Draw(instance_count_, 0, 1, 0);
        }
    );
}

} // namespace primal::graphics
