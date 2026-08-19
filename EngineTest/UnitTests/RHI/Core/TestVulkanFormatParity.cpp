/**
 * @file TestVulkanFormatParity.cpp
 * @brief P4c-F1 — DataFormat 映射补全的功能/视觉验收
 * @details 用例:
 *   1) StaticTraversal_NoUndefined — RHITypes.h DataFormat 全枚举遍历,
 *      除 Unknown / RGBA32_sRGB(双方共同不支持项)外无 VK_FORMAT_UNDEFINED。
 *   2) CreateSampledAndRT_EveryNewFormat — P4c-F1 新增的 36 个格式逐项创建
 *      64x64 采样纹理 + RT 纹理(用途允许时),零 validation error。
 *      设备不支持的组合(BC on MoltenVK 等)按 format properties 判定 skip。
 *   3) IntCopyRoundtripExact — UInt/SInt 家族 staging→texture→readback
 *      像素精确 roundtrip(整数值逐一相等,不经 SSIM)。
 *   4) RenderGradientPerFormat — 浮点家族渲染渐变+几何到各格式 RT →
 *      readback → RGBA8 → 方差断言 + 与 Metal 参照 SSIM(参照缺失时 skip)。
 *   5) RenderIntFormatsExact — UInt/SInt RT 渲染确定性整数图案,CPU 参照逐像素相等。
 *   6) Depth16PrePass — D16_UNorm 作为 DepthPrePass 深度附件完整跑通
 *      (clone 自 TestVulkanDepthPrePass 的 D32 流程)。
 *
 * 参照帧:Assets/ReferenceImages/P4c-F1/format_<name>.png(SimplePBR 约定:
 * 参照缺失时 skip 不 fail)。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanMath.h"
#endif

#include "Utils/ImageCompare.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64, kH = 64;

// Math helpers (column-major, matches math::m4x4 / std140 mat4).
// Mirrored from TestVulkanDepthPrePass.cpp — pure host-side UBO builders.
m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

m4x4 make_view_m4x4(v3 eye, v3 target, v3 up) {
    v3 diff = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    float zl = std::sqrt(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);
    if (zl < 1e-8f) zl = 1.0f;
    v3 zaxis = {diff[0]/zl, diff[1]/zl, diff[2]/zl};
    v3 x = {
        up[1]*zaxis[2] - up[2]*zaxis[1],
        up[2]*zaxis[0] - up[0]*zaxis[2],
        up[0]*zaxis[1] - up[1]*zaxis[0],
    };
    float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (xl < 1e-8f) xl = 1.0f;
    v3 xaxis = {x[0]/xl, x[1]/xl, x[2]/xl};
    v3 yaxis = {
        zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1],
        zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2],
        zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0],
    };
    m4x4 m = make_identity_m4x4();
    m.columns[0][0] = xaxis[0]; m.columns[0][1] = yaxis[0]; m.columns[0][2] = zaxis[0];
    m.columns[1][0] = xaxis[1]; m.columns[1][1] = yaxis[1]; m.columns[1][2] = zaxis[1];
    m.columns[2][0] = xaxis[2]; m.columns[2][1] = yaxis[1]; m.columns[2][2] = zaxis[2];
    m.columns[3][0] = -(xaxis[0]*eye[0] + xaxis[1]*eye[1] + xaxis[2]*eye[2]);
    m.columns[3][1] = -(yaxis[0]*eye[0] + yaxis[1]*eye[1] + yaxis[2]*eye[2]);
    m.columns[3][2] = -(zaxis[0]*eye[0] + zaxis[1]*eye[1] + zaxis[2]*eye[2]);
    m.columns[3][3] = 1.0f;
    return m;
}

m4x4 make_perspective_m4x4(float fov_y, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = f / aspect;
    r.columns[1][1] = f;
    r.columns[2][2] = (zFar + zNear) / (zNear - zFar);
    r.columns[2][3] = -1.0f;
    r.columns[3][2] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
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

/// P4c-F1 新增格式的元数据表。kind 决定 readback→RGBA8 转换与精确断言方式。
enum class FmtKind { UNorm16, SNorm16, UInt16, SInt16, UInt32, SInt32, Float32, Float16, Depth16, BC };
struct NewFormat {
    const char* name;
    DataFormat fmt;
    u32 channels;
    FmtKind kind;
};

const NewFormat kNewFormats[] = {
    // 深度
    { "D16_UNorm",       DataFormat::D16_UNorm,      1, FmtKind::Depth16 },
    // 16 位 norm/int
    { "R16_UNorm",       DataFormat::R16_UNorm,      1, FmtKind::UNorm16 },
    { "R16_SNorm",       DataFormat::R16_SNorm,      1, FmtKind::SNorm16 },
    { "R16_SInt",        DataFormat::R16_SInt,       1, FmtKind::SInt16 },
    { "RG16_UNorm",      DataFormat::RG16_UNorm,     2, FmtKind::UNorm16 },
    { "RG16_SNorm",      DataFormat::RG16_SNorm,     2, FmtKind::SNorm16 },
    { "RG16_SInt",       DataFormat::RG16_SInt,      2, FmtKind::SInt16 },
    { "RGBA16_UNorm",    DataFormat::RGBA16_UNorm,   4, FmtKind::UNorm16 },
    { "RGBA16_SNorm",    DataFormat::RGBA16_SNorm,   4, FmtKind::SNorm16 },
    { "RGBA16_UInt",     DataFormat::RGBA16_UInt,    4, FmtKind::UInt16 },
    { "RGBA16_SInt",     DataFormat::RGBA16_SInt,    4, FmtKind::SInt16 },
    // 32 位(UNorm/SNorm 映射到 SFLOAT — Metal 先例 fallback)
    { "R32_UNorm",       DataFormat::R32_UNorm,      1, FmtKind::Float32 },
    { "R32_SNorm",       DataFormat::R32_SNorm,      1, FmtKind::Float32 },
    { "R32_SInt",        DataFormat::R32_SInt,       1, FmtKind::SInt32 },
    { "RG32_UNorm",      DataFormat::RG32_UNorm,     2, FmtKind::Float32 },
    { "RG32_SNorm",      DataFormat::RG32_SNorm,     2, FmtKind::Float32 },
    { "RG32_UInt",       DataFormat::RG32_UInt,      2, FmtKind::UInt32 },
    { "RG32_SInt",       DataFormat::RG32_SInt,      2, FmtKind::SInt32 },
    { "RGB32_UNorm",     DataFormat::RGB32_UNorm,    3, FmtKind::Float32 },
    { "RGB32_SNorm",     DataFormat::RGB32_SNorm,    3, FmtKind::Float32 },
    { "RGB32_UInt",      DataFormat::RGB32_UInt,     3, FmtKind::UInt32 },
    { "RGB32_SInt",      DataFormat::RGB32_SInt,     3, FmtKind::SInt32 },
    { "RGBA32_UNorm",    DataFormat::RGBA32_UNorm,   4, FmtKind::Float32 },
    { "RGBA32_SNorm",    DataFormat::RGBA32_SNorm,   4, FmtKind::Float32 },
    { "RGBA32_UInt",     DataFormat::RGBA32_UInt,    4, FmtKind::UInt32 },
    { "RGBA32_SInt",     DataFormat::RGBA32_SInt,    4, FmtKind::SInt32 },
    // BC 全系(macOS 路径预期不可用 — 运行时 skip)
    { "BC1_sRGB",        DataFormat::BC1_sRGB,       4, FmtKind::BC },
    { "BC2_UNorm",       DataFormat::BC2_UNorm,      4, FmtKind::BC },
    { "BC2_sRGB",        DataFormat::BC2_sRGB,       4, FmtKind::BC },
    { "BC3_sRGB",        DataFormat::BC3_sRGB,       4, FmtKind::BC },
    { "BC4_UNorm",       DataFormat::BC4_UNorm,      1, FmtKind::BC },
    { "BC4_SNorm",       DataFormat::BC4_SNorm,      1, FmtKind::BC },
    { "BC5_SNorm",       DataFormat::BC5_SNorm,      2, FmtKind::BC },
    { "BC6H_UF16",       DataFormat::BC6H_UF16,      4, FmtKind::BC },
    { "BC6H_SF16",       DataFormat::BC6H_SF16,      4, FmtKind::BC },
    { "BC7_sRGB",        DataFormat::BC7_sRGB,       4, FmtKind::BC },
};
constexpr u32 kNewFormatCount = sizeof(kNewFormats) / sizeof(kNewFormats[0]);

/// 每 texel 字节(kind 推导,与 VulkanMath.h BytesPerTexel 一致)
u64 KindBytesPerTexel(FmtKind k, u32 channels) {
    switch (k) {
        case FmtKind::UNorm16: case FmtKind::SNorm16:
        case FmtKind::UInt16:  case FmtKind::SInt16:
        case FmtKind::Float16: case FmtKind::Depth16:
            return 2ull * channels;
        case FmtKind::UInt32:  case FmtKind::SInt32: case FmtKind::Float32:
            return 4ull * channels;
        case FmtKind::BC:
            return 0;  // block-compressed:不走 roundtrip 路径
    }
    return 0;
}

bool FormatSupports(VulkanDevice* vk, VkFormat fmt, VkFormatFeatureFlags bits) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(vk->GetNativePhysicalDevice(), fmt, &fp);
    return (fp.optimalTilingFeatures & bits) == bits;
}

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
}

/// 单 channel 原始值 → [0,255] 字节(浮点家族)
u8 ChannelToByte(FmtKind kind, const u8* raw) {
    auto u16v = [&]() -> u16 { u16 v; std::memcpy(&v, raw, 2); return v; };
    auto s16v = [&]() -> s16 { s16 v; std::memcpy(&v, raw, 2); return v; };
    auto f32v = [&]() -> float { float v; std::memcpy(&v, raw, 4); return v; };
    float c = 0.0f;
    switch (kind) {
        case FmtKind::UNorm16: c = u16v() / 65535.0f; break;
        case FmtKind::SNorm16: { float s = s16v() / 32767.0f; c = s < 0 ? 0 : s; } break;
        case FmtKind::Float32: c = f32v(); break;
        default: c = 0; break;
    }
    if (c < 0) c = 0;
    if (c > 1) c = 1;
    return static_cast<u8>(c * 255.0f + 0.5f);
}

} // anonymous namespace

// ============================================================================
// Case 1: 静态遍历 — 全枚举无 UNDEFINED(除双方共同不支持项)
// ============================================================================
TestResult TestStaticTraversal_NoUndefined() {
    // RHITypes.h DataFormat 枚举值 0..79(Unknown=0, RGBA32_sRGB=61)。
    // P4c-F1 契约:除 Unknown 与 RGBA32_sRGB(两边都缺)外全部映射。
    u32 undefinedCount = 0;
    for (u32 i = 0; i <= 79; ++i) {
        DataFormat fmt = static_cast<DataFormat>(i);
        VkFormat vkf = vulkan::ToVkFormat(fmt);
        if (vkf == VK_FORMAT_UNDEFINED) {
            if (i == 0 || i == 61) continue;  // 允许
            std::cerr << "[TestVulkanFormatParity] enum " << i
                      << " unexpectedly maps to UNDEFINED" << std::endl;
            ++undefinedCount;
        }
    }
    // 双方共同不支持项必须保持 UNDEFINED(错误映射同样是对等缺口)
    TEST_ASSERT(vulkan::ToVkFormat(DataFormat::Unknown) == VK_FORMAT_UNDEFINED,
                "Unknown must map to UNDEFINED");
    TEST_ASSERT(vulkan::ToVkFormat(DataFormat::RGBA32_sRGB) == VK_FORMAT_UNDEFINED,
                "RGBA32_sRGB has no VkFormat equivalent (both sides lack it)");
    TEST_ASSERT(undefinedCount == 0, "All other DataFormat values must map to valid VkFormat");
    std::cout << "[TestVulkanFormatParity] static traversal: 78/78 mapped (excl. Unknown, RGBA32_sRGB)"
              << std::endl;
    return TestResult::Passed;
}

// ============================================================================
// Case 2: 每个新增格式创建 64x64 采样纹理 + RT 纹理(用途允许时)
// ============================================================================
TestResult TestCreateSampledAndRT_EveryNewFormat() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    u32 sampledOK = 0, sampledSkip = 0, rtOK = 0, rtSkip = 0, failures = 0;

    for (u32 f = 0; f < kNewFormatCount; ++f) {
        const NewFormat& nf = kNewFormats[f];
        VkFormat vkf = vulkan::ToVkFormat(nf.fmt);
        TEST_ASSERT(vkf != VK_FORMAT_UNDEFINED, "new format must be mapped");

        const bool isDepth = (nf.kind == FmtKind::Depth16);
        const bool isBC = (nf.kind == FmtKind::BC);

        // --- 采样纹理 ---
        if (FormatSupports(fx.vk, vkf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !isBC) {
            TextureDesc td{};
            td.size = { kW, kH, 1 };
            td.mipLevels = 1;
            td.arraySize = 1;
            td.format = nf.fmt;
            td.type = TextureType::Texture2D;
            td.usage = TextureUsage::ShaderResource;
            td.memoryUsage = GPUMemoryUsage::Static;
            td.name = "FmtParity_Sampled";
            ResourceHandle h = fx.base->CreateTexture(td);
            if (h == handles::INVALID_RESOURCE) {
                std::cerr << "[TestVulkanFormatParity] sampled create FAILED: " << nf.name << std::endl;
                ++failures;
            } else {
                ++sampledOK;
                fx.base->DestroyTexture(h);
            }
        } else {
            ++sampledSkip;
            std::cout << "[TestVulkanFormatParity] sampled skip: " << nf.name
                      << (isBC ? " (block-compressed on this device)" : " (no SAMPLED_BIT)")
                      << std::endl;
        }

        // --- RT 纹理(颜色 RT;D16 走 Case 6 的完整 DepthPrePass)---
        if (!isDepth && !isBC &&
            FormatSupports(fx.vk, vkf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            TextureDesc td{};
            td.size = { kW, kH, 1 };
            td.mipLevels = 1;
            td.arraySize = 1;
            td.format = nf.fmt;
            td.type = TextureType::Texture2D;
            td.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
            td.memoryUsage = GPUMemoryUsage::Static;
            td.name = "FmtParity_RT";
            ResourceHandle h = fx.base->CreateTexture(td);
            if (h == handles::INVALID_RESOURCE) {
                std::cerr << "[TestVulkanFormatParity] RT create FAILED: " << nf.name << std::endl;
                ++failures;
            } else {
                ++rtOK;
                fx.base->DestroyTexture(h);
            }
        } else if (!isDepth) {
            ++rtSkip;
        }
    }

    std::cout << "[TestVulkanFormatParity] creation: sampled OK=" << sampledOK
              << " skip=" << sampledSkip << " | RT OK=" << rtOK << " skip=" << rtSkip << std::endl;
    TEST_ASSERT(failures == 0, "no creation failures among supported formats");
    TEST_ASSERT(sampledOK + rtOK > 0, "at least some formats must be usable on this device");
    return TestResult::Passed;
}

// ============================================================================
// Case 3: UInt/SInt 家族 copy roundtrip 像素精确断言
// ============================================================================
TestResult TestIntCopyRoundtripExact() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer");
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd, "GetCommandBuffer");

    u32 tested = 0, skipped = 0, failures = 0;

    for (u32 f = 0; f < kNewFormatCount; ++f) {
        const NewFormat& nf = kNewFormats[f];
        const bool isUInt = nf.kind == FmtKind::UInt16 || nf.kind == FmtKind::UInt32;
        const bool isSInt = nf.kind == FmtKind::SInt16 || nf.kind == FmtKind::SInt32;
        if (!isUInt && !isSInt) continue;

        const u64 bpt = KindBytesPerTexel(nf.kind, nf.channels);
        const u64 rowBytes = u64(kW) * bpt;
        const u64 totalBytes = rowBytes * kH;
        VkFormat vkf = vulkan::ToVkFormat(nf.fmt);

        if (!FormatSupports(fx.vk, vkf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            ++skipped;
            std::cout << "[TestVulkanFormatParity] roundtrip skip (no SAMPLED_BIT): "
                      << nf.name << std::endl;
            continue;
        }

        // 确定性整数 pattern:texel i 通道 c = base + i*step + c*chanStep,
        // 16 位用 u16/s16 范围,32 位用全范围。
        std::vector<u8> pattern{std::vector<u8>(size_t(totalBytes), 0)};
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                u32 i = y * kW + x;
                u8* texel = pattern.data() + size_t(y * kW + x) * size_t(bpt);
                for (u32 c = 0; c < nf.channels; ++c) {
                    if (nf.kind == FmtKind::UInt16 || nf.kind == FmtKind::SInt16) {
                        if (isUInt) {
                            u16 v = static_cast<u16>((i * 37u + c * 11001u) & 0xFFFFu);
                            std::memcpy(texel + c * 2, &v, 2);
                        } else {
                            s16 v = static_cast<s16>(((i * 37 + c * 11001) & 0x7FFF) - 16384);
                            std::memcpy(texel + c * 2, &v, 2);
                        }
                    } else {
                        if (isUInt) {
                            u32 v = (i * 2654435761u + c * 40503u) ;  // Knuth multiplicative
                            std::memcpy(texel + c * 4, &v, 4);
                        } else {
                            s32 v = static_cast<s32>((i * 2654435761u + c * 40503u) & 0xFFFF) - 32768;
                            std::memcpy(texel + c * 4, &v, 4);
                        }
                    }
                }
            }
        }

        // staging + texture + readback
        BufferDesc sdesc{};
        sdesc.size = totalBytes;
        sdesc.type = BufferType::Raw;
        sdesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sdesc.name = "RoundtripStaging";
        ResourceHandle staging = fx.base->CreateBuffer(sdesc);
        TEST_ASSERT(staging != handles::INVALID_RESOURCE, "CreateBuffer staging");
        TEST_ASSERT(fx.base->UpdateBufferData(staging, pattern.data(), totalBytes, 0),
                    "UpdateBufferData staging");

        TextureDesc td{};
        td.size = { kW, kH, 1 };
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = nf.fmt;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "RoundtripTex";
        ResourceHandle tex = fx.base->CreateTexture(td);
        if (tex == handles::INVALID_RESOURCE) {
            std::cerr << "[TestVulkanFormatParity] roundtrip create FAILED: " << nf.name << std::endl;
            ++failures;
            fx.base->DestroyBuffer(staging);
            continue;
        }

        BufferDesc rdesc{};
        rdesc.size = totalBytes;
        rdesc.type = BufferType::Raw;
        rdesc.memoryUsage = GPUMemoryUsage::Readback;
        rdesc.name = "RoundtripReadback";
        ResourceHandle readback = fx.base->CreateBuffer(rdesc);
        TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kW, kH, 1};

        TEST_ASSERT(vcmd->Reset(), "Reset");
        TEST_ASSERT(vcmd->Begin(), "Begin");
        vcmd->CopyBufferToTexture(staging, tex, &region, 1);
        vcmd->CopyTextureToBuffer(tex, readback, &region, 1);
        TEST_ASSERT(vcmd->End(), "End");
        TEST_ASSERT(vcmd->Submit(0), "Submit");
        TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");

        void* mapped = fx.base->MapBuffer(readback, 0, totalBytes);
        TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
        if (std::memcmp(mapped, pattern.data(), size_t(totalBytes)) != 0) {
            std::cerr << "[TestVulkanFormatParity] roundtrip MISMATCH: " << nf.name << std::endl;
            ++failures;
        }
        fx.base->UnmapBuffer(readback);

        fx.base->DestroyBuffer(readback);
        fx.base->DestroyTexture(tex);
        fx.base->DestroyBuffer(staging);
        ++tested;
    }

    std::cout << "[TestVulkanFormatParity] int roundtrip: exact=" << tested
              << " skip=" << skipped << " fail=" << failures << std::endl;
    TEST_ASSERT(failures == 0, "all integer roundtrips must be bit-exact");
    TEST_ASSERT(tested > 0, "at least one integer format must be testable");
    fx.base->DestroyCommandBuffer(cmd);
    return TestResult::Passed;
}

// ============================================================================
// Case 4+5: 渲染到各格式 RT(浮点家族渐变 + 整数家族精确图案)
// ============================================================================
namespace {

/// 渲染一个 fullscreen triangle 到 RT,readback 原始 texel 数据
std::vector<u8> RenderAndReadback(DeviceFixture& fx, VulkanCommandBuffer* vcmd,
                                  ShaderHandle vs, ShaderHandle fs, PipelineLayoutHandle pl,
                                  ResourceHandle rt, DataFormat rtFormat, u64 bytesPerTexel) {
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = rtFormat;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    if (pipe == handles::INVALID_PIPELINE) {
        std::cerr << "[TestVulkanFormatParity] CreateGraphicsPipeline failed" << std::endl;
        return {};
    }

    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = rtFormat;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};

    BufferDesc rdesc{};
    rdesc.size = u64(kW) * kH * bytesPerTexel;
    rdesc.type = BufferType::Raw;
    rdesc.memoryUsage = GPUMemoryUsage::Readback;
    rdesc.name = "RenderReadback";
    ResourceHandle readback = fx.base->CreateBuffer(rdesc);

    std::vector<u8> result;
    if (readback != handles::INVALID_RESOURCE &&
        vcmd->Reset() && vcmd->Begin()) {
        vcmd->BeginRenderPass(rpd);
        vcmd->BindGraphicsPipeline(pipe);
        vcmd->Draw(3, 0, 1, 0);
        vcmd->EndRenderPass();

        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kW, kH, 1};
        vcmd->CopyTextureToBuffer(rt, readback, &region, 1);

        if (vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion()) {
            void* mapped = fx.base->MapBuffer(readback, 0, rdesc.size);
            if (mapped) {
                result.resize(size_t(rdesc.size));
                std::memcpy(result.data(), mapped, size_t(rdesc.size));
                fx.base->UnmapBuffer(readback);
            }
        }
    }
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyPipeline(pipe);
    return result;
}

} // anonymous namespace

TestResult TestRenderGradientPerFormat() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/P4cFormatGradient.spv");
    auto frag = ReadSPV("Assets/Shaders/P4cFormatGradient.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read P4cFormatGradient SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader gradient vert");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader gradient frag");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout (empty)");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd, "GetCommandBuffer");

    u32 rendered = 0, skipped = 0, failures = 0;

    for (u32 f = 0; f < kNewFormatCount; ++f) {
        const NewFormat& nf = kNewFormats[f];
        const bool floatFamily = nf.kind == FmtKind::UNorm16 || nf.kind == FmtKind::SNorm16 ||
                                 nf.kind == FmtKind::Float32;
        if (!floatFamily) continue;
        VkFormat vkf = vulkan::ToVkFormat(nf.fmt);
        if (!FormatSupports(fx.vk, vkf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            ++skipped;
            std::cout << "[TestVulkanFormatParity] render skip (no COLOR_ATTACHMENT_BIT): "
                      << nf.name << std::endl;
            continue;
        }

        TextureDesc td{};
        td.size = { kW, kH, 1 };
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = nf.fmt;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "GradientRT";
        ResourceHandle rt = fx.base->CreateTexture(td);
        if (rt == handles::INVALID_RESOURCE) {
            ++failures;
            continue;
        }

        const u64 bpt = KindBytesPerTexel(nf.kind, nf.channels);
        std::vector<u8> raw = RenderAndReadback(fx, vcmd, vs, fs, pl, rt, nf.fmt, bpt);
        fx.base->DestroyTexture(rt);
        if (raw.empty()) {
            std::cerr << "[TestVulkanFormatParity] render/readback FAILED: " << nf.name << std::endl;
            ++failures;
            continue;
        }

        // raw → RGBA8
        std::vector<u8> rgba8(size_t(kW) * kH * 4, 255);
        for (u32 p = 0; p < kW * kH; ++p) {
            const u8* texel = raw.data() + size_t(p) * size_t(bpt);
            for (u32 c = 0; c < nf.channels; ++c) {
                rgba8[p * 4 + c] = ChannelToByte(nf.kind, texel + size_t(c) * (bpt / nf.channels));
            }
        }
        et::FlipYInPlace(rgba8.data(), kW, kH);

        // 方差断言:非全黑/全白(直方图方差 > 阈值)
        double mean = 0;
        for (u32 p = 0; p < kW * kH * 4; ++p) mean += rgba8[p];
        mean /= double(kW * kH * 4);
        double var = 0;
        for (u32 p = 0; p < kW * kH * 4; ++p) var += (rgba8[p] - mean) * (rgba8[p] - mean);
        var /= double(kW * kH * 4);
        if (var < 100.0) {
            std::cerr << "[TestVulkanFormatParity] gradient variance too low for "
                      << nf.name << ": " << var << std::endl;
            ++failures;
            continue;
        }

        char outPath[256];
        std::snprintf(outPath, sizeof(outPath), "P4c-F1/format_%s_vulkan.png", nf.name);
        std::error_code ec;
        std::filesystem::create_directories("P4c-F1", ec);
        et::SavePNG(outPath, rgba8.data(), kW, kH);

        // Metal 参照(缺失时 skip — SimplePBR 约定)
        char refPath[256];
        std::snprintf(refPath, sizeof(refPath), "Assets/ReferenceImages/P4c-F1/format_%s.png", nf.name);
        std::vector<u8> ref;
        u32 rw = 0, rh = 0;
        if (et::LoadPNG(refPath, ref, rw, rh) && rw == kW && rh == kH) {
            float ssim = et::ComputeSSIM(rgba8.data(), ref.data(), kW, kH);
            std::cout << "[TestVulkanFormatParity] " << nf.name
                      << " SSIM vs Metal ref: " << ssim << std::endl;
            if (ssim < 0.98f) {
                std::cerr << "[TestVulkanFormatParity] SSIM < 0.98 for " << nf.name << std::endl;
                ++failures;
            }
        }
        ++rendered;
    }

    std::cout << "[TestVulkanFormatParity] gradient render: OK=" << rendered
              << " skip=" << skipped << " fail=" << failures << std::endl;
    TEST_ASSERT(failures == 0, "no gradient render failures");
    TEST_ASSERT(rendered > 0, "at least one float format must be renderable");
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    fx.base->DestroyCommandBuffer(cmd);
    return TestResult::Passed;
}

TestResult TestRenderIntFormatsExact() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/P4cFormatGradient.spv");
    auto fragU = ReadSPV("Assets/Shaders/P4cFormatGradientUInt.frag.spv");
    auto fragS = ReadSPV("Assets/Shaders/P4cFormatGradientSInt.frag.spv");
    TEST_ASSERT(!vert.empty() && !fragU.empty() && !fragS.empty(), "Read int gradient SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fsU = fx.base->CreateShader(fragU.data(), fragU.size(), ShaderStage::Pixel, "main");
    ShaderHandle fsS = fx.base->CreateShader(fragS.data(), fragS.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fsU != handles::INVALID_SHADER &&
                fsS != handles::INVALID_SHADER, "CreateShader int gradients");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);

    u32 tested = 0, skipped = 0, failures = 0;

    for (u32 f = 0; f < kNewFormatCount; ++f) {
        const NewFormat& nf = kNewFormats[f];
        const bool isUInt = nf.kind == FmtKind::UInt16 || nf.kind == FmtKind::UInt32;
        const bool isSInt = nf.kind == FmtKind::SInt16 || nf.kind == FmtKind::SInt32;
        if (!isUInt && !isSInt) continue;
        VkFormat vkf = vulkan::ToVkFormat(nf.fmt);
        if (!FormatSupports(fx.vk, vkf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            ++skipped;
            continue;
        }

        TextureDesc td{};
        td.size = { kW, kH, 1 };
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = nf.fmt;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        td.memoryUsage = GPUMemoryUsage::Static;
        td.name = "IntGradientRT";
        ResourceHandle rt = fx.base->CreateTexture(td);
        if (rt == handles::INVALID_RESOURCE) { ++failures; continue; }

        const u64 bpt = KindBytesPerTexel(nf.kind, nf.channels);
        std::vector<u8> raw = RenderAndReadback(fx, vcmd, vs, isUInt ? fsU : fsS, pl, rt, nf.fmt, bpt);
        fx.base->DestroyTexture(rt);
        if (raw.empty()) { ++failures; continue; }

        // CPU 参照:shader 输出 uvec4(p.x&255, p.y&255, 128, 65535) /
        // ivec4(p.x&127, p.y&127, -128, 32767),按通道位宽截断后逐一相等。
        u32 mismatches = 0;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const u8* texel = raw.data() + size_t(y * kW + x) * size_t(bpt);
                // Vulkan framebuffer 坐标原点在左上;shader 用 gl_FragCoord
                // 与 readback 行序一致(未 FlipY),CPU 参照直接用 (x,y)。
                const u32 want[4] = { isUInt ? (x & 255) : (x & 127),
                                      isUInt ? (y & 255) : (y & 127),
                                      isUInt ? 128u : 0xFFFFFF80u,
                                      isUInt ? 65535u : 32767u };
                for (u32 c = 0; c < nf.channels; ++c) {
                    if (nf.kind == FmtKind::UInt16) {
                        u16 got; std::memcpy(&got, texel + c * 2, 2);
                        if (got != static_cast<u16>(want[c])) ++mismatches;
                    } else if (nf.kind == FmtKind::SInt16) {
                        s16 got; std::memcpy(&got, texel + c * 2, 2);
                        if (got != static_cast<s16>(static_cast<u32>(want[c]) & 0xFFFF) &&
                            got != static_cast<s16>(want[c] & 0xFFFF)) ++mismatches;
                    } else if (nf.kind == FmtKind::UInt32) {
                        u32 got; std::memcpy(&got, texel + c * 4, 4);
                        if (got != want[c]) ++mismatches;
                    } else {
                        s32 got; std::memcpy(&got, texel + c * 4, 4);
                        if (got != static_cast<s32>(want[c])) ++mismatches;
                    }
                }
            }
        }
        if (mismatches != 0) {
            std::cerr << "[TestVulkanFormatParity] int render mismatch " << nf.name
                      << ": " << mismatches << " channels differ" << std::endl;
            ++failures;
        }
        ++tested;
    }

    std::cout << "[TestVulkanFormatParity] int render exact: OK=" << tested
              << " skip=" << skipped << " fail=" << failures << std::endl;
    TEST_ASSERT(failures == 0, "integer renders must be pixel-exact");
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fsS);
    fx.base->DestroyShader(fsU);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

// ============================================================================
// Case 6: D16_UNorm DepthPrePass(clone 自 TestVulkanDepthPrePass 的 D32 流程)
// ============================================================================
namespace {
constexpr u32 kVertexStride = 32;
struct SphereMesh {
    std::vector<u8> vertices;
    std::vector<u32> indices;
    u32 vertexCount{0}, indexCount{0};
};
SphereMesh make_sphere(float radius, u32 segments, u32 rings) {
    SphereMesh s;
    s.vertexCount = (rings + 1) * (segments + 1);
    s.indexCount = rings * segments * 6;
    s.vertices.resize(size_t(s.vertexCount) * kVertexStride);
    s.indices.resize(s.indexCount);
    auto write_v = [&](u32 i, float px, float py, float pz,
                       float nx, float ny, float nz, float u, float v) {
        u8* p = s.vertices.data() + size_t(i) * kVertexStride;
        std::memcpy(p + 0, &px, 4); std::memcpy(p + 4, &py, 4); std::memcpy(p + 8, &pz, 4);
        std::memcpy(p + 12, &nx, 4); std::memcpy(p + 16, &ny, 4); std::memcpy(p + 20, &nz, 4);
        std::memcpy(p + 24, &u, 4); std::memcpy(p + 28, &v, 4);
    };
    u32 vi = 0;
    for (u32 r = 0; r <= rings; ++r) {
        float phi = 3.14159265358979f * float(r) / float(rings);
        float sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 seg = 0; seg <= segments; ++seg) {
            float theta = 2.0f * 3.14159265358979f * float(seg) / float(segments);
            float nx = sinP * std::cos(theta), ny = cosP, nz = sinP * std::sin(theta);
            write_v(vi, nx * radius, ny * radius, nz * radius, nx, ny, nz,
                    float(seg) / float(segments), float(r) / float(rings));
            ++vi;
        }
    }
    u32 ii = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 seg = 0; seg < segments; ++seg) {
            u32 a = r * (segments + 1) + seg;
            u32 b = a + segments + 1;
            s.indices[ii++] = a;     s.indices[ii++] = b; s.indices[ii++] = a + 1;
            s.indices[ii++] = a + 1; s.indices[ii++] = b; s.indices[ii++] = b + 1;
        }
    }
    return s;
}
} // anonymous namespace

TestResult TestDepth16PrePass() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // D16 能力检查:optimalTiling 需 DEPTH_STENCIL_ATTACHMENT
    if (!FormatSupports(fx.vk, VK_FORMAT_D16_UNORM, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        std::cout << "[TestVulkanFormatParity] D16 lacks DEPTH_STENCIL_ATTACHMENT — skip"
                  << std::endl;
        return TestResult::Skipped;
    }

    auto vert = ReadSPV("Assets/Shaders/CameraDepth.spv");
    auto frag = ReadSPV("Assets/Shaders/CameraDepth.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read CameraDepth SPIR-V");
    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "camera_depth_vs");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");

    SphereMesh sphere = make_sphere(1.0f, 24, 16);

    BufferDesc vbufDesc{};
    vbufDesc.size = sphere.vertices.size();
    vbufDesc.type = BufferType::Vertex;
    vbufDesc.vertex.vertexCount = sphere.vertexCount;
    vbufDesc.vertex.vertexStride = kVertexStride;
    vbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vbufDesc.name = "SphereVB";
    ResourceHandle vb = fx.base->CreateBuffer(vbufDesc);
    TEST_ASSERT(fx.base->UpdateBufferData(vb, sphere.vertices.data(), sphere.vertices.size(), 0),
                "UpdateBufferData vertex");

    BufferDesc ibufDesc{};
    ibufDesc.size = sphere.indices.size() * sizeof(u32);
    ibufDesc.type = BufferType::Index;
    ibufDesc.index.indexCount = sphere.indexCount;
    ibufDesc.index.format = DataFormat::R32_UInt;
    ibufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ibufDesc.name = "SphereIB";
    ResourceHandle ib = fx.base->CreateBuffer(ibufDesc);
    TEST_ASSERT(fx.base->UpdateBufferData(ib, sphere.indices.data(), ibufDesc.size, 0),
                "UpdateBufferData index");

    // 视角与 TestVulkanDepthPrePass 相同:eye (0,0,3),45° FOV — 参照帧可比。
    m4x4 view = make_view_m4x4(v3{0.0f, 0.0f, 3.0f}, v3{0.0f, 0.0f, 0.0f}, v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = make_perspective_m4x4(0.785398f, 1.0f, 0.1f, 100.0f);
    m4x4 wvp = proj * view;
    struct CameraDepthPerObject { m4x4 worldViewProjection; } uboBytes{};
    uboBytes.worldViewProjection = wvp;
    BufferDesc ubufDesc{};
    ubufDesc.size = sizeof(CameraDepthPerObject);
    ubufDesc.type = BufferType::Constant;
    ubufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubufDesc.name = "CameraDepthUBO";
    ResourceHandle ubo = fx.base->CreateBuffer(ubufDesc);
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &uboBytes, sizeof(uboBytes), 0), "UpdateBufferData UBO");

    DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = ShaderStage::Vertex;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 1;
    layoutDesc.bindings = &binding;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    DescriptorBufferInfo uboInfo{};
    uboInfo.buffer = ubo; uboInfo.offset = 0; uboInfo.range = sizeof(uboBytes);
    WriteDescriptorSet write{};
    write.dstSet = ds; write.dstBinding = 0; write.dstArrayElement = 0;
    write.descriptorCount = 1; write.descriptorType = DescriptorType::UniformBuffer;
    write.bufferInfo = &uboInfo;
    fx.base->UpdateDescriptorSets(1, &write);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.vertexAttributes.resize(5);
    gpd.vertexAttributes[0] = {0, 0, DataFormat::RGB32_Float, 0};
    gpd.vertexAttributes[1] = {1, 0, DataFormat::R32_UInt, 12};
    gpd.vertexAttributes[2] = {2, 0, DataFormat::R32_UInt, 16};
    gpd.vertexAttributes[3] = {3, 0, DataFormat::R32_UInt, 20};
    gpd.vertexAttributes[4] = {4, 0, DataFormat::RG32_Float, 24};
    gpd.vertexBindings.resize(1);
    gpd.vertexBindings[0].binding = 0;
    gpd.vertexBindings[0].stride = kVertexStride;
    gpd.vertexBindings[0].perVertex = true;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 0;
    gpd.depthStencilFormat = DataFormat::D16_UNorm;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline (D16 depth-only)");

    // D16 深度 RT
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::D16_UNorm;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::DepthStencil | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "D16DepthPrePass_RT";
    ResourceHandle rt = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture D16 RT");

    VulkanTexture* vtex = fx.vk->GetTexture(rt);
    TEST_ASSERT(vtex->GetAspectMask() == VK_IMAGE_ASPECT_DEPTH_BIT,
                "D16 aspect must be DEPTH_BIT (P4c-F1)");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 2;  // D16 = 2 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "D16Depth_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);

    RenderPassDesc rpd{};
    rpd.colorAttachments.clear();
    rpd.depthAttachment.texture = rt;
    rpd.depthAttachment.format = DataFormat::D16_UNorm;
    rpd.depthAttachment.loadOp = LoadAction::Clear;
    rpd.depthAttachment.storeOp = StoreAction::Store;
    rpd.depthAttachment.clearValue.depth = 1.0f;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Reset/Begin");
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    vcmd->BindVertexBuffers(0, 1, vbArr, vbOffsets);
    vcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
    vcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    vcmd->EndRenderPass();

    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(rt, readback, &region, 1);
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit D16 depth pass");

    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer D16 readback");
    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::Depth16ToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);
    et::FlipYInPlace(rgba8.data(), kW, kH);
    fx.base->UnmapBuffer(readback);

    u32 fg = 0;
    for (u32 i = 0; i < kW * kH; ++i) {
        if (rgba8[i * 4] < 255) ++fg;
    }
    std::cout << "[TestVulkanFormatParity] D16 depth fg pixels: " << fg << " / " << kW * kH << std::endl;
    TEST_ASSERT(fg > (kW * kH) / 4, "sphere covers > 1/4 of D16 depth frame");
    TEST_ASSERT(fg < (kW * kH * 9) / 10, "sphere must not fill D16 depth frame");
    {
        std::error_code ec;
        std::filesystem::create_directories("P4c-F1", ec);
    }
    et::SavePNG("P4c-F1/depth16_sphere_vulkan.png", rgba8.data(), kW, kH);

    // Metal D16 参照(缺失时 skip)
    std::vector<u8> ref;
    u32 rw = 0, rh = 0;
    if (et::LoadPNG("Assets/ReferenceImages/P4c-F1/depth16_sphere_metal.png", ref, rw, rh)
        && rw == kW && rh == kH) {
        float ssim = et::ComputeSSIM(rgba8.data(), ref.data(), kW, kH);
        std::cout << "[TestVulkanFormatParity] D16 SSIM vs Metal ref: " << ssim << std::endl;
        TEST_ASSERT(ssim >= 0.98f, "D16 depth parity SSIM >= 0.98");
    } else {
        std::cerr << "[TestVulkanFormatParity] D16 Metal reference missing — skipping SSIM"
                  << std::endl;
    }

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyBuffer(ib);
    fx.base->DestroyBuffer(vb);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanFormatParityTests() {
    auto suite = std::make_shared<TestSuite>("VulkanFormatParityTests");
    suite->AddTestCase(TestCase("StaticTraversal_NoUndefined",    TestStaticTraversal_NoUndefined));
    suite->AddTestCase(TestCase("CreateSampledAndRT_EveryNewFormat", TestCreateSampledAndRT_EveryNewFormat));
    suite->AddTestCase(TestCase("IntCopyRoundtripExact",          TestIntCopyRoundtripExact));
    suite->AddTestCase(TestCase("RenderGradientPerFormat",        TestRenderGradientPerFormat));
    suite->AddTestCase(TestCase("RenderIntFormatsExact",          TestRenderIntFormatsExact));
    suite->AddTestCase(TestCase("Depth16PrePass",                 TestDepth16PrePass));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanFormatParityTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanFormatParity] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
