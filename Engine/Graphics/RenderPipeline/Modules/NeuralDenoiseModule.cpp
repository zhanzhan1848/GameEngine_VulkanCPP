/**
 * @file NeuralDenoiseModule.cpp
 * @brief M1 神经降噪推理实现
 * @details 范式与 NeuralCaptureModule 一致（自加载字节码 / 持久纹理 /
 *          手工 UA⇄SRV 屏障 / 惰性 staging dump）。层描述符 init 期写死、
 *          每帧零更新：只有 pack pass 绑定 RG 导入的 GBuffer/SPGI 纹理。
 */
#include "NeuralDenoiseModule.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "Engine/Common/CoreLogStream.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
namespace primal::graphics {

using namespace rhi;
using namespace rendergraph;

namespace {

// 与 NeuralConv.comp / NeuralDenoise.metal 的 CB 逐字节镜像。
struct NeuralConvCB {
    math::v4 layer0;   // x=in_c y=out_c z=w_offset(f32 idx) w=b_offset
    math::v4 layer1;   // x=kernel y=pad z=flags(bit0=expm1) w=net_residual
    math::v4 res;      // x=w y=h
};
static_assert(sizeof(NeuralConvCB) == 48, "NeuralConvCB layout mismatch");

struct NeuralPackCB {
    math::v4 res_misc;   // x=w y=h z=depth_norm_inv w=unused
};
static_assert(sizeof(NeuralPackCB) == 16, "NeuralPackCB layout mismatch");

constexpr u32 kMaxConvChannels = 24;   // conv 层通道上限（kernel 寄存器数组）
constexpr float kDepthNorm = 1.0f / 16.0f;

std::vector<u8> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

std::vector<u8> LoadWithFallback(const std::string& rel) {
    std::vector<u8> bytes = ReadFileBytes(rel);
    for (int i = 1; i <= 6 && bytes.empty(); ++i) {
        std::string pfx;
        for (int j = 0; j < i; ++j) pfx += "../";
        bytes = ReadFileBytes(pfx + rel);
    }
    if (bytes.empty()) bytes = ReadFileBytes("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + rel);
    return bytes;
}

std::vector<u8> LoadNeuralBytecode(rhi::RHIDeviceBase* device, std::string name) {
    const auto platform = device ? device->GetPlatform() : RHIPlatform::Metal;
    if (platform == RHIPlatform::Vulkan) {
        return LoadWithFallback("Engine/Graphics/Vulkan/shaders/Neural/" + name);
    }
    if (platform == RHIPlatform::Metal) {
        return LoadWithFallback("Engine/Graphics/Metal/shaders/Neural/" + name + ".metal");
    }
    return {};
}

void InsertStateBarrier(RHICommandBuffer* cmd, ResourceHandle res,
                        ResourceState before, ResourceState after) {
    ResourceBarrier b{};
    b.resource = res;
    b.beforeState = before;
    b.afterState = after;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&b, 1);
}

// IEEE-754 binary16 ⇄ binary32（MLX 路径的回读/回传编解码；denoised_ 与
// pack 纹理都是 rgba16f）。舍入：就近偶数。
u16 F32ToF16(f32 v) {
    u32 x;
    std::memcpy(&x, &v, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    u32 exp = (x >> 23) & 0xFFu;
    u32 man = x & 0x7FFFFFu;
    if (exp == 0xFF) {   // Inf/NaN
        const u16 nan = man ? 0x7E00u : 0x7C00u;
        return static_cast<u16>(sign | nan);
    }
    int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 0x1F) return static_cast<u16>(sign | 0x7C00u);      // 溢出 → Inf
    if (e <= 0) {                                                // 亚正规/零
        if (e < -10) return static_cast<u16>(sign);
        man = (man | 0x800000u) >> (1 - e);
        return static_cast<u16>(sign + ((man + 0x1000) >> 13));
    }
    u32 h = sign | (static_cast<u32>(e) << 10) + ((man + 0x1000) >> 13);
    return static_cast<u16>(h);
}

f32 F16ToF32(u16 h) {
    const u32 sign = (h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 man = h & 0x3FFu;
    u32 out;
    if (exp == 0) {
        if (man == 0) {
            out = sign;
        } else {   // 亚正规
            int e = -1;
            u32 m = man;
            do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
            m &= 0x3FFu;
            out = sign | (static_cast<u32>(127 - 15 + e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (man << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    f32 v;
    std::memcpy(&v, &out, 4);
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool NeuralDenoiseModule::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    if (!device) return false;

    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;

    if (const char* e = std::getenv("PRIMAL_NEURAL_DENOISE_DUMP")) {
        if (*e) dump_path_ = e;
    }
    dump_frame_ = 300;
    if (const char* e = std::getenv("PRIMAL_NEURAL_DENOISE_DUMP_FRAME")) {
        const int v = std::atoi(e);
        if (v > 0) dump_frame_ = static_cast<u32>(v);
    }

    // ---- 权重 ----
    std::string pnn_path;
    if (const char* e = std::getenv("PRIMAL_NEURAL_DENOISE_PNN")) {
        if (*e) pnn_path = e;
    }
    neural::NeuralNetwork net;
    std::string err;
    bool loaded = false;
    if (!pnn_path.empty()) {
        loaded = neural::LoadPnnFile(pnn_path, net, &err);
    } else {
        for (const char* c : {"EngineTest/assets/Neural/denoiser.pnn",
                              "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/denoiser.pnn"}) {
            if ((loaded = neural::LoadPnnFile(c, net, &err))) {
                pnn_path = c;
                break;
            }
        }
    }
    if (!loaded) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] pnn load failed (" << err
                  << ") — disabled" << std::endl;
        return false;
    }
    if (net.layers.empty()) return false;
    for (const neural::LayerDesc& d : net.layers) {
        if (d.kind != neural::LayerKind::Conv || d.out_dim > kMaxConvChannels) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] unsupported layer topology"
                      << std::endl;
            return false;
        }
    }
    layer_count_ = (u32)net.layers.size();
    weights_float_count_ = (u32)net.weights.size();

    // ---- 推理后端选择（路线修正：MLX 优先可选，kernel 保底）----
    // PRIMAL_NEURAL_DENOISE_BACKEND = mlx | kernel（默认 kernel）。
    const char* backend_env = std::getenv("PRIMAL_NEURAL_DENOISE_BACKEND");
    const std::string backend = backend_env ? backend_env : "kernel";
    if (backend == "mlx") {
        std::string mlx_err;
        if (device->GetPlatform() != RHIPlatform::Metal) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[NeuralDenoise] backend=mlx 需要 Metal 平台——降级 kernel" << std::endl;
        } else if (mlx_backend_.Initialize({}, {}, &mlx_err)) {
            use_mlx_ = true;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[NeuralDenoise] MLX backend: " << mlx_backend_.LoadedDylibPath()
                << " | " << mlx_backend_.LoadedWeightsPath() << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[NeuralDenoise] MLX backend init failed (" << mlx_err
                << ")——降级 kernel 路径" << std::endl;
        }
    }

    // ---- 字节码 ----
    auto make_shader = [&](const char* vk_name, const char* mtl_stem,
                           const char* entry) -> ShaderHandle {
        std::vector<u8> bytes;
        if (isVk) bytes = LoadNeuralBytecode(device, vk_name);
        else bytes = LoadNeuralBytecode(device, mtl_stem);
        if (bytes.empty()) return handles::INVALID_SHADER;
        return device->CreateShader(bytes.data(), bytes.size(), ShaderStage::Compute, entry);
    };
    ShaderHandle pack_cs, conv_cs;
    if (isVk) {
        pack_cs = make_shader("NeuralDenoisePack.comp.spv", "", "main");
        conv_cs = make_shader("NeuralConv.comp.spv", "", "main");
    } else {
        pack_cs = make_shader("", "NeuralDenoise", "neural_denoise_pack");
        conv_cs = make_shader("", "NeuralDenoise", "neural_conv");
    }
    if (pack_cs == handles::INVALID_SHADER || conv_cs == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] shader bytecode missing" << std::endl;
        return false;
    }

    // ---- 布局/管线 ----
    if (isVk) {
        DescriptorSetLayoutBinding pb[] = {
            {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            // 2026-09-23 修正：GLSL 声明 outT0 为 binding 3 StorageImage
            // （NeuralDenoisePack.comp:21），原 SampledImage 声明是笔误——
            // UpdateDescriptorSets 写 StorageImage 到 binding 3 时触发
            // descriptorType mismatch 校验错误。
            {3, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
        };
        // 注意 gi 在 GLSL 是 binding 0（SampledImage）、depth binding 1。
        pb[0] = {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr};
        pb[1] = {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr};
        pack_set_layout_ = device->CreateDescriptorSetLayout({6, pb});
        DescriptorSetLayoutBinding cb_[15];   // 15 条目（数组必须 15，勿回 14）
        for (u32 i = 0; i < 6; ++i) cb_[i] = {i, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        for (u32 i = 6; i < 12; ++i) cb_[i] = {i, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        cb_[12] = {12, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};   // 残差源
        cb_[13] = {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};   // weights (SSBO)
        cb_[14] = {14, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};   // CB
        conv_set_layout_ = device->CreateDescriptorSetLayout({15, cb_});
    } else {
        DescriptorSetLayoutBinding pb[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // gi
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // A0
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // A1
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        pack_set_layout_ = device->CreateDescriptorSetLayout({6, pb});
        DescriptorSetLayoutBinding cb_[15];   // 15 条目（数组必须 15，勿回 14）
        // Metal：纹理 0..5 = in（texture 命名空间）、6..11 = out、buffer 0 = weights、
        // buffer 1 = CB。binding 0/1 在两个命名空间各有一条目（与 GIGather 同惯例），
        // MetalDescriptorSet 按 (binding, type) 匹配。
        for (u32 i = 0; i < 6; ++i) cb_[i] = {i, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        for (u32 i = 6; i < 12; ++i) cb_[i] = {i, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        cb_[12] = {12, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // 残差源 (texture 12)
        cb_[13] = {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};   // weights (buffer 0)
        cb_[14] = {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};   // CB (buffer 1)
        conv_set_layout_ = device->CreateDescriptorSetLayout({15, cb_});
    }
    pack_layout_ = device->CreatePipelineLayout({1, &pack_set_layout_});
    conv_layout_ = device->CreatePipelineLayout({1, &conv_set_layout_});
    ComputePipelineDesc ppd{}; ppd.computeShader = pack_cs; ppd.layout = pack_layout_;
    ppd.threadGroupSize = {8, 8, 1};
    pack_pipeline_ = device->CreateComputePipeline(ppd);
    ComputePipelineDesc cpd{}; cpd.computeShader = conv_cs; cpd.layout = conv_layout_;
    cpd.threadGroupSize = {8, 8, 1};
    conv_pipeline_ = device->CreateComputePipeline(cpd);
    if (pack_pipeline_ == handles::INVALID_PIPELINE ||
        conv_pipeline_ == handles::INVALID_PIPELINE) {
        return false;
    }
    for (u32 i = 0; i < 3; ++i) pack_sets_[i] = device->CreateDescriptorSet({pack_set_layout_});
    for (u32 i = 0; i < 4; ++i) conv_sets_[i] = device->CreateDescriptorSet({conv_set_layout_});

    // ---- 资源 ----
    TextureDesc td{};
    td.size = {render_width_, render_height_, 1};
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    td.format = DataFormat::RGBA16_Float;
    td.name = "NeuralDn_Bank";
    for (u32 i = 0; i < 6; ++i) {
        bank_a_[i] = device->CreateTexture(td);
        bank_b_[i] = device->CreateTexture(td);
    }
    // MLX 路径要 CPU 回读 pack 输出（CopySource）；kernel 路径的 dump 快照
    // 也走同一用法（Metal validation 对 usage 语义有断言，见下 denoised_）。
    if (use_mlx_) td.usage = td.usage | TextureUsage::CopySource;
    td.name = "NeuralDn_Pack";
    pack_p0_ = device->CreateTexture(td);
    pack_p1_ = device->CreateTexture(td);
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
               TextureUsage::CopySource;
    if (use_mlx_) td.usage = td.usage | TextureUsage::CopyDest;   // CopyBufferToTexture 回传
    td.name = "NeuralDn_Out";
    denoised_ = device->CreateTexture(td);
    td.size = {1, 1, 1};
    // kernel 声明了对全部 6 张输出纹理的写访问——占位纹理也要 ShaderWrite，
    // 否则 Metal validation 断言（usage 0x01 != ShaderWrite）直接中止。
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    td.name = "NeuralDn_Dummy";
    dummy_tex_ = device->CreateTexture(td);

    BufferDesc wDesc{};
    wDesc.size = (u64)net.weights.size() * 4ull;
    wDesc.type = BufferType::Structured;
    wDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    wDesc.usage = GPUMemoryUsage::Dynamic;
    wDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    wDesc.name = "NeuralDn_Weights";
    weights_buf_ = device->CreateBuffer(wDesc);
    if (weights_buf_ == handles::INVALID_RESOURCE) return false;
    if (auto* dst = static_cast<f32*>(device->MapBuffer(weights_buf_))) {
        std::memcpy(dst, net.weights.data(), net.weights.size() * 4);
        device_->UnmapBuffer(weights_buf_);
    }

    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    cbDesc.name = "NeuralDn_CB";
    for (u32 i = 0; i < 4; ++i) layer_cb_[i] = device->CreateBuffer(cbDesc);
    for (u32 i = 0; i < 3; ++i) pack_cb_[i] = device->CreateBuffer(cbDesc);

    // 层 CB（静态）：{in,out,w_off,b_off} {k,pad,expm1,net_residual} {w,h}
    for (u32 i = 0; i < layer_count_; ++i) {
        const neural::LayerDesc& d = net.layers[i];
        if (auto* cb = static_cast<NeuralConvCB*>(device->MapBuffer(layer_cb_[i]))) {
            cb->layer0 = math::v4{float(d.in_dim), float(d.out_dim),
                                  float(d.w_offset), float(d.b_offset)};
            // 2026-09-15：末层 expm1 后写 rgba16f，值 >65504 溢出为 INF——
            // 改为输出 log 域（fp16 安全），expm1 由消费端（Fusion/调试显示）
            // 执行。剥离 expm1 位，保留残差位（residual 在 log 域相加）。
            cb->layer1 = math::v4{float(d.kernel), float(d.pad),
                                  0.0f,
                                  (net.flags & 1u) ? 1.0f : 0.0f};
            cb->res = math::v4{float(render_width_), float(render_height_), 0.0f, 0.0f};
            device_->UnmapBuffer(layer_cb_[i]);
        }
    }
    for (u32 i = 0; i < 3; ++i) {
        if (auto* cb = static_cast<NeuralPackCB*>(device->MapBuffer(pack_cb_[i]))) {
            cb->res_misc = math::v4{float(render_width_), float(render_height_),
                                    kDepthNorm, 0.0f};
            device_->UnmapBuffer(pack_cb_[i]);
        }
    }

    // ---- 层描述符（init 写死；conv 层每帧零更新）----
    // L0: A→B；L1: B→A；L2: A→B；L3: B→denoised。bank 内未用通道绑 dummy。
    const u32 layer_ubo_vk = 13, layer_ubo_mtl = 1;
    rhi::ResourceHandle in_l0[6] = {pack_p0_, pack_p1_, dummy_tex_, dummy_tex_,
                                    dummy_tex_, dummy_tex_};
    auto bind_bank_in = [&](DescriptorSetHandle set, const rhi::ResourceHandle* tex) {
        WriteDescriptorSet w[6] = {};
        DescriptorImageInfo infos[6];
        for (u32 i = 0; i < 6; ++i) {
            infos[i].imageView = tex[i];
            infos[i].imageLayout = ResourceState::ShaderResource;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = DescriptorType::SampledImage;
            w[i].imageInfo = &infos[i];
        }
        device->UpdateDescriptorSets(6, w);
    };
    auto bind_bank_out = [&](DescriptorSetHandle set, rhi::ResourceHandle const* bank) {
        WriteDescriptorSet w[6] = {};
        DescriptorImageInfo* infos = new DescriptorImageInfo[6];
        for (u32 i = 0; i < 6; ++i) {
            infos[i].imageView = bank[i];
            infos[i].imageLayout = ResourceState::UnorderedAccess;
            w[i].dstSet = set;
            w[i].dstBinding = 6 + i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = DescriptorType::StorageImage;
            w[i].imageInfo = &infos[i];
        }
        device->UpdateDescriptorSets(6, w);
        delete[] infos;
    };
    bind_bank_in(conv_sets_[0], in_l0);
    bind_bank_out(conv_sets_[0], bank_b_);
    bind_bank_in(conv_sets_[1], bank_b_);
    bind_bank_out(conv_sets_[1], bank_a_);
    bind_bank_in(conv_sets_[2], bank_a_);
    bind_bank_out(conv_sets_[2], bank_b_);
    bind_bank_in(conv_sets_[3], bank_b_);
    // 残差源（binding 14 / texture 12）：L0..L2 绑 dummy，L3 绑 P0
    {
        WriteDescriptorSet w[4] = {};
        DescriptorImageInfo* infos = new DescriptorImageInfo[4];
        for (u32 i = 0; i < 4; ++i) {
            infos[i].imageView = (i == 3) ? pack_p0_ : dummy_tex_;
            infos[i].imageLayout = ResourceState::ShaderResource;
            w[i].dstSet = conv_sets_[i];
            w[i].dstBinding = isVk ? 14u : 12u;
            w[i].descriptorCount = 1;
            w[i].descriptorType = DescriptorType::SampledImage;
            w[i].imageInfo = &infos[i];
        }
        device->UpdateDescriptorSets(4, w);
        delete[] infos;
    }
    // L3 输出只有 denoised 一张，其余 5 槽绑 dummy。
    {
        WriteDescriptorSet w[6] = {};
        DescriptorImageInfo* infos = new DescriptorImageInfo[6];
        for (u32 i = 0; i < 6; ++i) {
            infos[i].imageView = (i == 0) ? denoised_ : dummy_tex_;
            infos[i].imageLayout = i == 0 ? ResourceState::UnorderedAccess
                                          : ResourceState::ShaderResource;
            w[i].dstSet = conv_sets_[3];
            w[i].dstBinding = 6 + i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = DescriptorType::StorageImage;
            w[i].imageInfo = &infos[i];
        }
        device->UpdateDescriptorSets(6, w);
        delete[] infos;
    }
    // weights + CB 到 4 个层 set（binding 12/13 vk，buffer 0/1 mtl）
    for (u32 i = 0; i < 4; ++i) {
        DescriptorBufferInfo wInfo{weights_buf_, 0, (u64)net.weights.size() * 4ull};
        DescriptorBufferInfo cInfo{layer_cb_[i], 0, sizeof(NeuralConvCB)};
        WriteDescriptorSet w[2] = {};
        w[0].dstSet = conv_sets_[i];
        w[0].dstBinding = isVk ? 12 : 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = DescriptorType::StorageBuffer;
        w[0].bufferInfo = &wInfo;
        w[1].dstSet = conv_sets_[i];
        w[1].dstBinding = isVk ? 13 : 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = DescriptorType::UniformBuffer;
        w[1].bufferInfo = &cInfo;
        device->UpdateDescriptorSets(2, w);
    }

    if (!dump_path_.empty()) EnsureStaging();

    // ---- MLX 路径环形缓冲（3 槽 = 2 帧裕量）----
    if (use_mlx_) {
        const u64 tex_bytes = static_cast<u64>(render_width_) * render_height_ * 8ull;
        for (u32 i = 0; i < 3; ++i) {
            BufferDesc rb{};
            rb.size = tex_bytes * 2ull;   // P0 + P1（各 rgba16f 平铺）
            rb.usage = GPUMemoryUsage::Readback;
            rb.memoryUsage = GPUMemoryUsage::Readback;
            rb.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
            rb.name = "NeuralDn_MlxReadback";
            mlx_readback_[i] = device->CreateBuffer(rb);

            BufferDesc up{};
            up.size = tex_bytes;
            up.usage = GPUMemoryUsage::Dynamic;
            up.memoryUsage = GPUMemoryUsage::Dynamic;
            up.bindFlags = static_cast<u32>(BufferUsageFlags::TransferSrc);
            up.name = "NeuralDn_MlxUpload";
            mlx_upload_[i] = device->CreateBuffer(up);
        }
        mlx_in_.resize((size_t)net.input_dim * render_width_ * render_height_);
        mlx_out_.resize((size_t)net.output_dim * render_width_ * render_height_);
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralDenoise] initialized: pnn=" << pnn_path
              << " layers=" << layer_count_ << " in=" << net.input_dim
              << " out=" << net.output_dim << " weights=" << weights_float_count_
              << " f32 backend=" << (use_mlx_ ? "mlx" : "kernel") << std::endl;
    return true;
}

void NeuralDenoiseModule::Shutdown() {
    if (device_ == nullptr) return;
    for (u32 i = 0; i < 6; ++i) {
        if (bank_a_[i] != handles::INVALID_RESOURCE) device_->DestroyTexture(bank_a_[i]);
        if (bank_b_[i] != handles::INVALID_RESOURCE) device_->DestroyTexture(bank_b_[i]);
        bank_a_[i] = bank_b_[i] = handles::INVALID_RESOURCE;
    }
    if (denoised_ != handles::INVALID_RESOURCE) device_->DestroyTexture(denoised_);
    denoised_ = handles::INVALID_RESOURCE;
    if (dummy_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(dummy_tex_);
    dummy_tex_ = handles::INVALID_RESOURCE;
    if (weights_buf_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(weights_buf_);
    weights_buf_ = handles::INVALID_RESOURCE;
    for (u32 i = 0; i < 4; ++i) {
        if (layer_cb_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(layer_cb_[i]);
        layer_cb_[i] = handles::INVALID_RESOURCE;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (pack_cb_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(pack_cb_[i]);
        pack_cb_[i] = handles::INVALID_RESOURCE;
    }
    if (staging_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(staging_);
    staging_ = handles::INVALID_RESOURCE;
    for (u32 i = 0; i < 3; ++i) {
        if (mlx_readback_[i] != handles::INVALID_RESOURCE)
            device_->DestroyBuffer(mlx_readback_[i]);
        if (mlx_upload_[i] != handles::INVALID_RESOURCE)
            device_->DestroyBuffer(mlx_upload_[i]);
        mlx_readback_[i] = mlx_upload_[i] = handles::INVALID_RESOURCE;
        mlx_pending_[i] = mlx_slot_done_[i] = false;
    }
    mlx_backend_.Shutdown();
    use_mlx_ = false;
    conv_pipeline_ = handles::INVALID_PIPELINE;
    pack_pipeline_ = handles::INVALID_PIPELINE;
    device_ = nullptr;
}

bool NeuralDenoiseModule::EnsureStaging() {
    if (staging_ != handles::INVALID_RESOURCE) return true;
    staging_size_ = static_cast<u64>(render_width_) * render_height_ * 8ull * 16ull;   // [denoised|T0|T1|B0..B5|A0..A5(L1)] RGBA16F
    BufferDesc d{};
    d.size = staging_size_;
    d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
    d.usage = GPUMemoryUsage::Readback;
    d.memoryUsage = GPUMemoryUsage::Readback;
    d.name = "NeuralDn_Staging";
    staging_ = device_->CreateBuffer(d);
    return staging_ != handles::INVALID_RESOURCE;
}

// ---------------------------------------------------------------------------
// AddPass：pack → 4 层 conv（单 RG pass，多层 dispatch + 手工屏障）
// ---------------------------------------------------------------------------
void NeuralDenoiseModule::AddPass(RenderGraph& graph, const NeuralDenoiseInputs& inputs) {
    if (!IsInitialized()) return;
    const u32 cbIdx = inputs.current_buffer_index % 3;
    ++tick_;
    if (tick_ == 1 || tick_ == 303 || tick_ == 306) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] AddPass tick=" << tick_
                  << " gi_valid=" << inputs.spgi_gi_output.IsValid() << std::endl;
    }

    auto resolvePhys = [&graph](RGResourceHandle h) -> ResourceHandle {
        auto* r = graph.GetResource(h);
        return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
    };

    graph.AddPass<int>(
        "NeuralDenoise",
        RGPassType::Compute,
        RGPassCategory::Lighting,
        [](int&, RenderGraphBuilder& builder) { builder.SideEffect(); },
        [this, inputs, cbIdx](const int&, RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;
            auto phys = [&](RGResourceHandle rg) -> ResourceHandle {
                auto* r = ctx.graph->GetResource(rg);
                return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            };
            const ResourceHandle gi = phys(inputs.spgi_gi_output);
            const ResourceHandle depth = phys(inputs.gbuffer_depth);
            const ResourceHandle normal = phys(inputs.gbuffer_normal);
            if (gi == handles::INVALID_RESOURCE || depth == handles::INVALID_RESOURCE ||
                normal == handles::INVALID_RESOURCE) {
                static bool logged_fail = false;
                if (!logged_fail) { logged_fail = true;
                    PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] resolve fail" << std::endl; }
                return;
            }
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            static bool logged_exec = false;
            if (!logged_exec) { logged_exec = true;
                PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] exec OK" << std::endl; }
            const u32 w = inputs.render_width ? inputs.render_width : render_width_;
            const u32 h = inputs.render_height ? inputs.render_height : render_height_;

            // ---- MLX 回传：上一轮该槽的推理结果 → denoised_ ----
            const u32 mlx_slot = tick_ % 3;
            if (use_mlx_ && mlx_pending_[mlx_slot]) {
                InsertStateBarrier(cmd, denoised_, ResourceState::ShaderResource,
                                   ResourceState::CopyDest);
                BufferTextureCopyRegion up{};
                up.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                up.imageExtent = Extent3D{w, h, 1};
                cmd->CopyBufferToTexture(mlx_upload_[mlx_slot], denoised_, &up, 1);
                InsertStateBarrier(cmd, denoised_, ResourceState::CopyDest,
                                   ResourceState::ShaderResource);
                mlx_pending_[mlx_slot] = false;
            }

            // ---- pack ----
            DescriptorImageInfo giInfo; giInfo.imageView = gi; giInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo dInfo; dInfo.imageView = depth; dInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo nInfo; nInfo.imageView = normal; nInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo a0Info; a0Info.imageView = pack_p0_; a0Info.imageLayout = ResourceState::UnorderedAccess;
            DescriptorImageInfo a1Info; a1Info.imageView = pack_p1_; a1Info.imageLayout = ResourceState::UnorderedAccess;
            DescriptorBufferInfo pcInfo; pcInfo.buffer = pack_cb_[cbIdx]; pcInfo.offset = 0;
            pcInfo.range = sizeof(NeuralPackCB);
            WriteDescriptorSet pw[6];
            const DescriptorType sampled = DescriptorType::SampledImage;
            pw[0].dstSet = pack_sets_[cbIdx]; pw[0].dstBinding = 0;
            pw[0].descriptorCount = 1; pw[0].descriptorType = sampled; pw[0].imageInfo = &giInfo;
            pw[1].dstSet = pack_sets_[cbIdx]; pw[1].dstBinding = 1;
            pw[1].descriptorCount = 1;
            pw[1].descriptorType = isVk ? DescriptorType::SampledDepthImage : sampled;
            pw[1].imageInfo = &dInfo;
            pw[2].dstSet = pack_sets_[cbIdx]; pw[2].dstBinding = 2;
            pw[2].descriptorCount = 1; pw[2].descriptorType = sampled; pw[2].imageInfo = &nInfo;
            pw[3].dstSet = pack_sets_[cbIdx]; pw[3].dstBinding = 3;
            pw[3].descriptorCount = 1; pw[3].descriptorType = DescriptorType::StorageImage;
            pw[3].imageInfo = &a0Info;
            pw[4].dstSet = pack_sets_[cbIdx]; pw[4].dstBinding = 4;
            pw[4].descriptorCount = 1; pw[4].descriptorType = DescriptorType::StorageImage;
            pw[4].imageInfo = &a1Info;
            pw[5].dstSet = pack_sets_[cbIdx];
            pw[5].dstBinding = isVk ? 5u : 0u;
            pw[5].descriptorCount = 1; pw[5].descriptorType = DescriptorType::UniformBuffer;
            pw[5].bufferInfo = &pcInfo;
            device_->UpdateDescriptorSets(6, pw);

            // gi 上一阶段 UA 写出 → SRV；depth/normal 已是 SRV。
            InsertStateBarrier(cmd, gi, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            cmd->BindComputePipeline(pack_pipeline_);
            const DescriptorSetHandle psets[] = {pack_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, pack_layout_, 0, 1, psets, 0, nullptr);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

            // ---- MLX 路径：pack 输出 CPU 回读，跳过 conv dispatch ----
            if (use_mlx_) {
                InsertStateBarrier(cmd, pack_p0_, ResourceState::UnorderedAccess,
                                   ResourceState::CopySource);
                InsertStateBarrier(cmd, pack_p1_, ResourceState::UnorderedAccess,
                                   ResourceState::CopySource);
                BufferTextureCopyRegion r{};
                r.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                r.imageExtent = Extent3D{w, h, 1};
                r.bufferOffset = 0;
                cmd->CopyTextureToBuffer(pack_p0_, mlx_readback_[mlx_slot], &r, 1);
                r.bufferOffset = static_cast<u64>(w) * h * 8ull;
                cmd->CopyTextureToBuffer(pack_p1_, mlx_readback_[mlx_slot], &r, 1);
                InsertStateBarrier(cmd, pack_p0_, ResourceState::CopySource,
                                   ResourceState::UnorderedAccess);
                InsertStateBarrier(cmd, pack_p1_, ResourceState::CopySource,
                                   ResourceState::UnorderedAccess);
                // MLX 路径没有 dump 流（对拍/PSNR 走 TestNeuralMLXShim 与
                // 离线 dump 工具）；CPU 侧推理在 AddPass 尾部。
                return;
            }

            // ---- 4 层 conv：读前写后交替，手工 UA→SRV 屏障 ----
            // B0 快照占位：实际拷贝在 L0 dispatch 之后（见下）
            if (!dump_path_.empty() && tick_ == 8) {
                // 输入快照（pack 输出 A0/A1 被 L1 覆写前拷走）
                InsertStateBarrier(cmd, pack_p0_, ResourceState::UnorderedAccess, ResourceState::CopySource);
                InsertStateBarrier(cmd, pack_p1_, ResourceState::UnorderedAccess, ResourceState::CopySource);
                BufferTextureCopyRegion r2{};
                r2.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                r2.imageExtent = Extent3D{w, h, 1};
                r2.bufferOffset = static_cast<u64>(w) * h * 8ull;
                cmd->CopyTextureToBuffer(pack_p0_, staging_, &r2, 1);
                r2.bufferOffset = static_cast<u64>(w) * h * 8ull * 2ull;
                cmd->CopyTextureToBuffer(pack_p1_, staging_, &r2, 1);
                InsertStateBarrier(cmd, pack_p0_, ResourceState::CopySource, ResourceState::UnorderedAccess);
                InsertStateBarrier(cmd, pack_p1_, ResourceState::CopySource, ResourceState::UnorderedAccess);
            }
            InsertStateBarrier(cmd, pack_p0_, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            InsertStateBarrier(cmd, pack_p1_, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            cmd->BindComputePipeline(conv_pipeline_);
            for (u32 l = 0; l < layer_count_; ++l) {
                const DescriptorSetHandle csets[] = {conv_sets_[l]};
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, conv_layout_, 0, 1, csets, 0, nullptr);
                cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                // 本层输出（B 组在 l 偶数、A 组在 l 奇数）降 SRV 供下一层读。
                if (l == 0 && !dump_path_.empty() && tick_ == 8) {
                    BufferTextureCopyRegion r4{};
                    r4.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                    r4.imageExtent = Extent3D{w, h, 1};
                    for (u32 t = 0; t < 6; ++t) {
                        InsertStateBarrier(cmd, bank_b_[t], ResourceState::UnorderedAccess, ResourceState::CopySource);
                        r4.bufferOffset = static_cast<u64>(w) * h * 8ull * (3ull + t);
                        cmd->CopyTextureToBuffer(bank_b_[t], staging_, &r4, 1);
                        InsertStateBarrier(cmd, bank_b_[t], ResourceState::CopySource, ResourceState::UnorderedAccess);
                    }
                }
                if (l == 1 && !dump_path_.empty() && tick_ == 8) {
                    // L1 输出 A0..A5 → plane 9..14
                    for (u32 t = 0; t < 6; ++t) {
                        InsertStateBarrier(cmd, bank_a_[t], ResourceState::UnorderedAccess, ResourceState::CopySource);
                        BufferTextureCopyRegion r5{};
                        r5.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                        r5.imageExtent = Extent3D{w, h, 1};
                        r5.bufferOffset = static_cast<u64>(w) * h * 8ull * (9ull + t);
                        cmd->CopyTextureToBuffer(bank_a_[t], staging_, &r5, 1);
                        InsertStateBarrier(cmd, bank_a_[t], ResourceState::CopySource, ResourceState::UnorderedAccess);
                    }
                }
                if (l + 1 < layer_count_) {
                    const bool wrote_b = (l % 2 == 0);
                    for (u32 i = 0; i < 6; ++i)
                        InsertStateBarrier(cmd, wrote_b ? bank_b_[i] : bank_a_[i],
                                           ResourceState::UnorderedAccess,
                                           ResourceState::ShaderResource);
                }
            }
            InsertStateBarrier(cmd, denoised_, ResourceState::UnorderedAccess, ResourceState::ShaderResource);

            // ---- dump（PRIMAL_NEURAL_DENOISE_DUMP）----
            if (!dump_path_.empty() && !dump_done_) {
                if (tick_ == dump_frame_ && !dump_copied_) {
                    InsertStateBarrier(cmd, denoised_, ResourceState::ShaderResource, ResourceState::CopySource);
                    BufferTextureCopyRegion region{};
                    region.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                    region.imageExtent = Extent3D{w, h, 1};
                    cmd->CopyTextureToBuffer(denoised_, staging_, &region, 1);
                    InsertStateBarrier(cmd, denoised_, ResourceState::CopySource, ResourceState::ShaderResource);
                    dump_copied_ = true;
                    map_tick_ = tick_ + 5;
                }
            }
        });

    // dump 回读（CPU，+5 帧）
    if (!dump_path_.empty() && dump_copied_ && !dump_done_ && tick_ >= map_tick_) {
        if (auto* texels = static_cast<const u16*>(device_->MapBuffer(staging_))) {
            std::ofstream out(dump_path_, std::ios::binary);
            out.write(reinterpret_cast<const char*>(texels), (std::streamsize)staging_size_);
            device_->UnmapBuffer(staging_);
            PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralDenoise] dump written: " << dump_path_
                      << " (" << staging_size_ << " B RGBA16F)" << std::endl;
        }
        dump_done_ = true;
    }

    // MLX 路径：CPU 侧回读→推理→回传准备（2 帧前的槽）。MapBuffer 会等
    // GPU 完成，裕量槽避免每帧 stall。
    if (use_mlx_ && tick_ >= 3) {
        const u32 done_slot = (tick_ - 2) % 3;
        if (!mlx_slot_done_[done_slot]) {
            const u32 w = render_width_, h = render_height_;
            mlx_pending_[done_slot] = MlxProcessSlot(done_slot, w, h);
            mlx_slot_done_[done_slot] = true;   // 失败只尝试一次，槽停用
        }
    }
}

bool NeuralDenoiseModule::MlxProcessSlot(u32 slot, u32 w, u32 h) {
    const size_t plane = static_cast<size_t>(w) * h;
    auto* raw = static_cast<const u16*>(device_->MapBuffer(mlx_readback_[slot]));
    if (!raw) return false;
    // P0 平铺在前，P1 在 w*h*8 偏移；conv 输入 7ch = P0.rgba + P1.rgb。
    for (size_t i = 0; i < plane; ++i) {
        const u16* p0 = raw + i * 4;
        const u16* p1 = raw + plane * 4 + i * 4;
        mlx_in_[0 * plane + i] = F16ToF32(p0[0]);
        mlx_in_[1 * plane + i] = F16ToF32(p0[1]);
        mlx_in_[2 * plane + i] = F16ToF32(p0[2]);
        mlx_in_[3 * plane + i] = F16ToF32(p0[3]);
        mlx_in_[4 * plane + i] = F16ToF32(p1[0]);
        mlx_in_[5 * plane + i] = F16ToF32(p1[1]);
        mlx_in_[6 * plane + i] = F16ToF32(p1[2]);
    }
    device_->UnmapBuffer(mlx_readback_[slot]);

    std::string err;
    // flags=0 → log 域输出（残差已加），与 kernel 路径现行消费端约定一致；
    // kernel 侧对输出做 max(v,0)，这里同样钳制保持两端一致。
    if (!mlx_backend_.Infer(mlx_in_.data(), 7, w, h, mlx_out_.data(), 3, 0u, &err)) {
        if (mlx_infer_err_.empty()) {
            mlx_infer_err_ = err;
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDenoise] MLX infer failed: "
                      << err << "——该槽停用" << std::endl;
        }
        return false;
    }

    auto* up = static_cast<u16*>(device_->MapBuffer(mlx_upload_[slot]));
    if (!up) return false;
    for (size_t i = 0; i < plane; ++i) {
        up[i * 4 + 0] = F32ToF16(std::max(mlx_out_[0 * plane + i], 0.0f));
        up[i * 4 + 1] = F32ToF16(std::max(mlx_out_[1 * plane + i], 0.0f));
        up[i * 4 + 2] = F32ToF16(std::max(mlx_out_[2 * plane + i], 0.0f));
        up[i * 4 + 3] = 0;
    }
    device_->UnmapBuffer(mlx_upload_[slot]);
    return true;
}

}  // namespace primal::graphics
