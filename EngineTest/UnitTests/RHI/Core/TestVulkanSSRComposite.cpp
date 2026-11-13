/**
 * @file TestVulkanSSRComposite.cpp
 * @brief Phase 4b Tier 3.6 — SSR Composite (full-res additive blend with Fresnel).
 * @details SSRComposite.wgsl reads HDR + half-res SSR + depth, reconstructs
 *          view normal from depth neighbors (cross product), applies Fresnel
 *          weight based on NdotV, and additively blends reflection into HDR.
 *
 *          Per-pixel math, no ray marching — tractable to write CPU reference.
 *
 * Setup:
 *   - 16×16 HDR (RGBA16F): known gradient
 *   - 8×8 SSR half-res (RGBA16F): known reflection colors
 *   - 16×16 depth (D32): linear-in-x gradient (ensures non-degenerate normals)
 *   - UBO: screenWidth/Height/halfWidth/halfHeight + fresnelPower +
 *          reflectionStrength + debugMode + pad + invProj mat4
 *
 * CPU reference mirrors WGSL exactly:
 *   - Sample SSR via bilinear at half-res
 *   - Reconstruct view pos + normal from depth (cross of neighbors)
 *   - Fresnel = pow(1 - NdotV, fresnelPower)
 *   - result = HDR + SSR * Fresnel * reflectionStrength
 *
 * Tolerance: per-pixel max abs diff < 0.02. Half-float precision + bilinear.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 16;
constexpr u32 kH = 16;
constexpr u32 kHalfW = kW / 2;
constexpr u32 kHalfH = kH / 2;
constexpr float kNear = 0.5f;
constexpr float kFar  = 10.0f;

u16 float_to_half(float f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    u32 sign = (bits >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    u32 mant = bits & 0x7FFFFF;
    if (exp <= 0) {
        if (exp < -10) return static_cast<u16>(sign);
        mant |= 0x800000;
        u32 shift = static_cast<u32>(14 - exp);
        return static_cast<u16>(sign | (mant >> shift));
    } else if (exp == 0xFF - (127 - 15)) {
        return static_cast<u16>(sign | 0x7C00 | (mant >> 13));
    } else if (exp > 31) {
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

float half_to_float(u16 h) {
    u32 sign = (h & 0x8000) << 16;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    auto bits_to_float = [](u32 b) {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    };
    if (exp == 0) {
        if (mant == 0) return bits_to_float(sign);
        while ((mant & 0x400) == 0) { mant <<= 1; exp -= 1; }
        exp += 1;
        mant &= 0x3FF;
    } else if (exp == 31) {
        return bits_to_float(sign | 0x7F800000 | (mant << 13));
    }
    u32 f_exp = exp + (127 - 15);
    u32 f_bits = sign | (f_exp << 23) | (mant << 13);
    return bits_to_float(f_bits);
}

// ============================= Test scene inputs =============================
// HDR color: pixel (x,y) = (x/W, y/H, 0.25, 1.0) in linear RGB.
void make_hdr(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float r = float(x) / float(kW - 1);
            float g = float(y) / float(kH - 1);
            float b = 0.25f;
            float a = 1.0f;
            u8* p = out.data() + (size_t(y) * kW + x) * 8;
            u16 hr = float_to_half(r), hg = float_to_half(g),
                hb = float_to_half(b), ha = float_to_half(a);
            std::memcpy(p + 0, &hr, 2);
            std::memcpy(p + 2, &hg, 2);
            std::memcpy(p + 4, &hb, 2);
            std::memcpy(p + 6, &ha, 2);
        }
    }
}

// SSR half-res: pixel (x,y) = (0.5, 0.0, 0.5, 1.0) — magenta-ish reflection.
void make_ssr(std::vector<u8>& out) {
    out.resize(size_t(kHalfW) * kHalfH * 8);
    for (u32 y = 0; y < kHalfH; ++y) {
        for (u32 x = 0; x < kHalfW; ++x) {
            u8* p = out.data() + (size_t(y) * kHalfW + x) * 8;
            float r = 0.5f, g = 0.0f, b = 0.5f, a = 1.0f;
            u16 hr = float_to_half(r), hg = float_to_half(g),
                hb = float_to_half(b), ha = float_to_half(a);
            std::memcpy(p + 0, &hr, 2);
            std::memcpy(p + 2, &hg, 2);
            std::memcpy(p + 4, &hb, 2);
            std::memcpy(p + 6, &ha, 2);
        }
    }
}

// Depth: linear-in-x gradient, never reaching sky (depth < 0.9999).
// depth(x, y) = 0.2 + 0.5 * (x / (W-1)). Range [0.2, 0.7].
void make_depth(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float d = 0.2f + 0.5f * (float(x) / float(kW - 1));
            u8* p = out.data() + (size_t(y) * kW + x) * 4;
            std::memcpy(p, &d, 4);
        }
    }
}

// ============================= Math helpers (CPU mirror of WGSL) =============================
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Mat4 { float m[16]; };  // column-major like WGSL mat4x4

Vec3 sub3(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float dot3(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross3(Vec3 a, Vec3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
float length3(Vec3 v) { return std::sqrt(dot3(v, v)); }
Vec3 normalize3(Vec3 v) {
    float l = length3(v);
    if (l < 1e-20f) return {0, 0, 0};
    return { v.x / l, v.y / l, v.z / l };
}

// invProj for ortho with L=-1, R=1, B=-1, T=1, N=kNear, F=kFar (Vulkan NDC z [0,1]).
// P column 2: [-1/(F-N), 0, 0, -N/(F-N)] — clip_z = -view_z/(F-N) - N/(F-N).
// Inverse: view_z = -NDC_z * (F-N) - N. Then shader returns -vp.z = NDC_z * (F-N) + N.
Mat4 make_inv_proj() {
    // invProj * (ndcX, ndcY, ndcZ, 1) = (ndcX, ndcY, -ndcZ*(F-N) - N, 1)
    // In column-major mat4x4 layout:
    //   col0 = (1, 0, 0, 0)
    //   col1 = (0, 1, 0, 0)
    //   col2 = (0, 0, -(F-N), 0)
    //   col3 = (0, 0, -N, 1)
    Mat4 m{};
    m.m[0] = 1;  m.m[5] = 1;
    m.m[10] = -(kFar - kNear);
    m.m[14] = -kNear;
    m.m[15] = 1;
    return m;
}

Vec4 mat4_mul_vec4(const Mat4& M, Vec4 v) {
    // Column-major: result[i] = sum_j M.col[j][i] * v[j]
    // M.m[col*4 + row]
    Vec4 r{};
    for (int row = 0; row < 4; ++row) {
        float s = 0;
        for (int col = 0; col < 4; ++col) {
            s += M.m[col * 4 + row] * (col == 0 ? v.x : (col == 1 ? v.y : (col == 2 ? v.z : v.w)));
        }
        if (row == 0) r.x = s;
        else if (row == 1) r.y = s;
        else if (row == 2) r.z = s;
        else r.w = s;
    }
    return r;
}

Vec3 reconstruct_view_pos(float uv_x, float uv_y, float ndcDepth, const Mat4& invProj) {
    float ndcX = uv_x * 2.0f - 1.0f;
    float ndcY = 1.0f - uv_y * 2.0f;
    Vec4 clipPos{ ndcX, ndcY, ndcDepth, 1.0f };
    Vec4 vp = mat4_mul_vec4(invProj, clipPos);
    // w=1 for ortho, but divide anyway for safety.
    Vec3 v = { vp.x / vp.w, vp.y / vp.w, vp.z / vp.w };
    return { v.x, v.y, -v.z };
}

float fetch_depth(const std::vector<u8>& depthPx, int x, int y) {
    x = std::clamp(x, 0, int(kW) - 1);
    y = std::clamp(y, 0, int(kH) - 1);
    float d;
    std::memcpy(&d, depthPx.data() + (size_t(y) * kW + x) * 4, 4);
    return d;
}

Vec3 reconstruct_view_normal(int px, int py, float uv_x, float uv_y, float depth,
                              const std::vector<u8>& depthPx, const Mat4& invProj) {
    float ts_x = 1.0f / float(kW);  // screenSize.z
    float ts_y = 1.0f / float(kH);  // screenSize.w
    int cx = std::clamp(px, 1, int(kW) - 2);
    int cy = std::clamp(py, 1, int(kH) - 2);
    float dL = fetch_depth(depthPx, cx - 1, cy);
    float dR = fetch_depth(depthPx, cx + 1, cy);
    float dU = fetch_depth(depthPx, cx, cy - 1);
    float dD = fetch_depth(depthPx, cx, cy + 1);
    Vec3 pL = reconstruct_view_pos(uv_x - ts_x, uv_y, dL, invProj);
    Vec3 pR = reconstruct_view_pos(uv_x + ts_x, uv_y, dR, invProj);
    Vec3 pU = reconstruct_view_pos(uv_x, uv_y - ts_y, dU, invProj);
    Vec3 pD = reconstruct_view_pos(uv_x, uv_y + ts_y, dD, invProj);
    Vec3 n = normalize3(cross3(sub3(pD, pU), sub3(pR, pL)));
    if (n.z > 0.0f) n = { -n.x, -n.y, -n.z };
    return n;
}

// Bilinear sample of half-res SSR at full-res UV.
Vec4 sample_bilinear_ssr(const std::vector<u8>& ssrPx, float uv_x, float uv_y) {
    float coord_x = uv_x * float(kHalfW) - 0.5f;
    float coord_y = uv_y * float(kHalfH) - 0.5f;
    int bx = std::clamp(int(std::floor(coord_x)), 0, int(kHalfW) - 1);
    int by = std::clamp(int(std::floor(coord_y)), 0, int(kHalfH) - 1);
    int bx1 = std::min(u32(bx) + 1u, kHalfW - 1u);
    int by1 = std::min(u32(by) + 1u, kHalfH - 1u);
    float fx = std::clamp(coord_x - std::floor(coord_x), 0.0f, 1.0f);
    float fy = std::clamp(coord_y - std::floor(coord_y), 0.0f, 1.0f);
    auto fetch = [&](int xi, int yi) -> Vec4 {
        const u8* p = ssrPx.data() + (size_t(yi) * kHalfW + xi) * 8;
        u16 hr, hg, hb, ha;
        std::memcpy(&hr, p + 0, 2);
        std::memcpy(&hg, p + 2, 2);
        std::memcpy(&hb, p + 4, 2);
        std::memcpy(&ha, p + 6, 2);
        return { half_to_float(hr), half_to_float(hg), half_to_float(hb), half_to_float(ha) };
    };
    Vec4 c00 = fetch(bx,  by);
    Vec4 c10 = fetch(bx1, by);
    Vec4 c01 = fetch(bx,  by1);
    Vec4 c11 = fetch(bx1, by1);
    Vec4 r;
    r.x = c00.x * (1 - fx) * (1 - fy) + c10.x * fx * (1 - fy) + c01.x * (1 - fx) * fy + c11.x * fx * fy;
    r.y = c00.y * (1 - fx) * (1 - fy) + c10.y * fx * (1 - fy) + c01.y * (1 - fx) * fy + c11.y * fx * fy;
    r.z = c00.z * (1 - fx) * (1 - fy) + c10.z * fx * (1 - fy) + c01.z * (1 - fx) * fy + c11.z * fx * fy;
    r.w = c00.w * (1 - fx) * (1 - fy) + c10.w * fx * (1 - fy) + c01.w * (1 - fx) * fy + c11.w * fx * fy;
    return r;
}

// ============================= SPIR-V / device helpers =============================
std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "ReadSPV: cannot open " << path << std::endl;
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
}

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

ResourceHandle MakeAndUploadTexture2D(DeviceFixture& fx, const std::vector<u8>& pixels,
                                       u32 w, u32 h, DataFormat format, TextureUsage extra,
                                       ResourceState finalState, const char* name) {
    TextureDesc td{};
    td.size = { w, h, 1 };
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = format;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource | extra;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    ResourceHandle tex = fx.base->CreateTexture(td);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc sd{};
    sd.size = pixels.size();
    sd.type = BufferType::Raw;
    sd.memoryUsage = GPUMemoryUsage::Dynamic;
    sd.name = "Stage";
    ResourceHandle staging = fx.base->CreateBuffer(sd);
    fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0);

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { w, h, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);

    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = finalState;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

} // anonymous namespace

TestResult TestVulkanSSRComposite() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto spv = ReadSPV("Assets/Shaders/SSRComposite.spv");
    TEST_ASSERT(!spv.empty(), "Read SSRComposite.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "ssr_composite");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    std::vector<u8> hdrPx, ssrPx, depthPx;
    make_hdr(hdrPx);
    make_ssr(ssrPx);
    make_depth(depthPx);

    ResourceHandle hdrTex = MakeAndUploadTexture2D(fx, hdrPx, kW, kH,
                                                    DataFormat::RGBA16_Float, TextureUsage{},
                                                    ResourceState::ShaderResource, "SSRComp_HDR");
    ResourceHandle ssrTex = MakeAndUploadTexture2D(fx, ssrPx, kHalfW, kHalfH,
                                                    DataFormat::RGBA16_Float, TextureUsage{},
                                                    ResourceState::ShaderResource, "SSRComp_SSR");
    ResourceHandle depthTex = MakeAndUploadTexture2D(fx, depthPx, kW, kH,
                                                      DataFormat::D32_Float, TextureUsage{},
                                                      ResourceState::ShaderResource, "SSRComp_Depth");
    TEST_ASSERT(hdrTex != handles::INVALID_RESOURCE, "hdrTex");
    TEST_ASSERT(ssrTex != handles::INVALID_RESOURCE, "ssrTex");
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "depthTex");

    ResourceHandle outputTex = fx.base->CreateTexture([&]{
        TextureDesc d{};
        d.size = { kW, kH, 1 };
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = DataFormat::RGBA16_Float;
        d.type = TextureType::Texture2D;
        d.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "SSRComp_Out";
        return d;
    }());
    TEST_ASSERT(outputTex != handles::INVALID_RESOURCE, "outputTex");

    // SSRCompositeParams: 4 u32 + 2 f32 + 2 u32 + mat4x4 invProj.
    // Layout (16-byte aligned, std140-compatible):
    //   offset 0:  screenWidth  (u32)
    //   offset 4:  screenHeight (u32)
    //   offset 8:  halfWidth    (u32)
    //   offset 12: halfHeight   (u32)
    //   offset 16: fresnelPower (f32)
    //   offset 20: reflectionStrength (f32)
    //   offset 24: debugMode    (u32)
    //   offset 28: _pad1        (u32)
    //   offset 32: invProj col0 (vec4)
    //   offset 48: invProj col1
    //   offset 64: invProj col2
    //   offset 80: invProj col3
    //   total: 96 bytes
    struct SSRCompositeParams {
        u32 screenWidth;
        u32 screenHeight;
        u32 halfWidth;
        u32 halfHeight;
        float fresnelPower;
        float reflectionStrength;
        u32 debugMode;
        u32 pad1;
        float invProj[16];  // column-major
    };
    SSRCompositeParams params{};
    params.screenWidth = kW;
    params.screenHeight = kH;
    params.halfWidth = kHalfW;
    params.halfHeight = kHalfH;
    params.fresnelPower = 5.0f;
    params.reflectionStrength = 0.5f;
    params.debugMode = 0;  // normal composite
    params.pad1 = 0;
    Mat4 invProj = make_inv_proj();
    std::memcpy(params.invProj, invProj.m, 64);

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(SSRCompositeParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "SSRComp_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer UBO");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &params, sizeof(params), 0), "UpdateBufferData UBO");

    DescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = { 0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[1] = { 1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[2] = { 2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[3] = { 3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr };
    bindings[4] = { 4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 5;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);

    DescriptorImageInfo hdrInfo{   handles::INVALID_SAMPLER, hdrTex,   ResourceState::ShaderResource };
    DescriptorImageInfo ssrInfo{   handles::INVALID_SAMPLER, ssrTex,   ResourceState::ShaderResource };
    DescriptorImageInfo depthInfo{ handles::INVALID_SAMPLER, depthTex, ResourceState::ShaderResource };
    DescriptorImageInfo outInfo{   handles::INVALID_SAMPLER, outputTex, ResourceState::UnorderedAccess };
    DescriptorBufferInfo uboInfo{ ubo, 0, sizeof(SSRCompositeParams) };
    WriteDescriptorSet writes[5]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage,  &hdrInfo,   nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::SampledImage,  &ssrInfo,   nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::SampledImage,  &depthInfo, nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::StorageImage,  &outInfo,   nullptr };
    writes[4] = { ds, 4, 0, 1, DescriptorType::UniformBuffer, nullptr,    &uboInfo };
    fx.base->UpdateDescriptorSets(5, writes);

    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();

    ResourceBarrier initBarrier{};
    initBarrier.resource = outputTex;
    initBarrier.beforeState = ResourceState::Unknown;
    initBarrier.afterState = ResourceState::UnorderedAccess;
    initBarrier.subresource = 0xFFFFFFFF;
    initBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Dispatch(kW / 8 + 1, kH / 8 + 1, 1);  // ceil-div for 16/8=2

    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = outputTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // Readback: RGBA16F = 8 bytes/pixel
    BufferDesc rbDesc{};
    rbDesc.size = u64(kW) * kH * 8;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "SSRComp_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);

    CommandBufferHandle cmd2 = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd2 = fx.vk->GetCommandBuffer(cmd2);
    vcmd2->Reset(); vcmd2->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { kW, kH, 1 };
    vcmd2->CopyTextureToBuffer(outputTex, readback, &region, 1);
    vcmd2->End(); vcmd2->Submit(0); vcmd2->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd2);

    void* mapped = fx.base->MapBuffer(readback, 0, u64(kW) * kH * 8);
    TEST_ASSERT(mapped, "MapBuffer");

    // CPU reference per pixel.
    float maxDiff = 0.0f, meanDiff = 0.0f;
    u32 count = 0;
    u32 worstX = 0, worstY = 0;
    float worstGPU[4]{}, worstCPU[4]{};
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = static_cast<const u8*>(mapped) + (size_t(y) * kW + x) * 8;
            u16 hr, hg, hb, ha;
            std::memcpy(&hr, p + 0, 2);
            std::memcpy(&hg, p + 2, 2);
            std::memcpy(&hb, p + 4, 2);
            std::memcpy(&ha, p + 6, 2);
            float gpuR = half_to_float(hr), gpuG = half_to_float(hg),
                  gpuB = half_to_float(hb), gpuA = half_to_float(ha);

            // CPU reference mirror of WGSL ssr_composite.
            float hdrR = float(x) / float(kW - 1);
            float hdrG = float(y) / float(kH - 1);
            float hdrB = 0.25f;
            float hdrA = 1.0f;

            float depth = fetch_depth(depthPx, int(x), int(y));

            float outR, outG, outB, outA;
            if (depth >= 0.9999f) {
                outR = hdrR; outG = hdrG; outB = hdrB; outA = hdrA;
            } else {
                float pixelUV_x = (float(x) + 0.5f) / float(kW);
                float pixelUV_y = (float(y) + 0.5f) / float(kH);
                Vec4 ssr = sample_bilinear_ssr(ssrPx, pixelUV_x, pixelUV_y);
                Vec3 n = reconstruct_view_normal(int(x), int(y), pixelUV_x, pixelUV_y,
                                                  depth, depthPx, invProj);
                Vec3 surfacePos = reconstruct_view_pos(pixelUV_x, pixelUV_y, depth, invProj);
                Vec3 V = normalize3({ -surfacePos.x, -surfacePos.y, -surfacePos.z });
                float NdotV = std::max(dot3(n, V), 0.0f);
                float fresnel = std::pow(1.0f - NdotV, params.fresnelPower);
                float strength = params.reflectionStrength;
                outR = hdrR + ssr.x * fresnel * strength;
                outG = hdrG + ssr.y * fresnel * strength;
                outB = hdrB + ssr.z * fresnel * strength;
                outA = hdrA;
            }

            float dR = std::abs(gpuR - outR);
            float dG = std::abs(gpuG - outG);
            float dB = std::abs(gpuB - outB);
            float dA = std::abs(gpuA - outA);
            float lm = std::max(dR, std::max(dG, std::max(dB, dA)));
            if (lm > maxDiff) {
                maxDiff = lm;
                worstX = x; worstY = y;
                worstGPU[0] = gpuR; worstGPU[1] = gpuG; worstGPU[2] = gpuB; worstGPU[3] = gpuA;
                worstCPU[0] = outR; worstCPU[1] = outG; worstCPU[2] = outB; worstCPU[3] = outA;
            }
            meanDiff += (dR + dG + dB + dA) / 4.0f;
            ++count;
        }
    }
    meanDiff /= float(count);
    fx.base->UnmapBuffer(readback);

    std::cout << "[TestVulkanSSRComposite] max diff=" << maxDiff
              << " mean=" << meanDiff << " texels=" << count << std::endl;
    std::cout << "[SSRComposite worst] x=" << worstX << " y=" << worstY
              << " GPU=(" << worstGPU[0] << "," << worstGPU[1] << "," << worstGPU[2] << "," << worstGPU[3] << ")"
              << " CPU=(" << worstCPU[0] << "," << worstCPU[1] << "," << worstCPU[2] << "," << worstCPU[3] << ")"
              << std::endl;

    // Tolerance: half-float precision + Fresnel pow precision.
    TEST_ASSERT(meanDiff < 0.005f, "mean abs diff < 0.005");
    TEST_ASSERT(maxDiff < 0.03f,   "max abs diff < 0.03 (half + pow precision)");

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyTexture(outputTex);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyTexture(ssrTex);
    fx.base->DestroyTexture(hdrTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanSSRComposite_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanSSRComposite_Tests");
    suite->AddTestCase(TestCase("SSRComposite", TestVulkanSSRComposite));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSSRComposite_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSSRComposite] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
