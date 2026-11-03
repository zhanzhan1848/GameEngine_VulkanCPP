/**
 * @file D3D12Abi.h
 * @brief D3D12 / DXGI 公开 ABI 常量表（平台中立，无任何 Windows 头）
 * @details D3D12 RHI 现代化 M0（F0-5）。
 *
 *          本文件手定义与 Microsoft 公开 ABI 文档一致的枚举位值，使
 *          平台中立核（Format/StateMapping/StateTracker/RootSignatureBudget）
 *          能在 macOS 上编译并单测（Master Plan §2 分层隔离约束）。
 *
 *          值来源（2026-11-03 全表重校，双源交叉核对）：
 *            a) Microsoft Learn 公开枚举页：
 *               - DXGI_FORMAT: .../dxgiformat/ne-dxgiformat-dxgi_format
 *               - D3D12_FILTER / D3D12_RESOURCE_STATES / D3D12_RESOURCE_DIMENSION /
 *                 D3D12_TEXTURE_ADDRESS_MODE / D3D12_COMPARISON_FUNC / D3D12_BLEND /
 *                 D3D12_BLEND_OP / D3D12_DESCRIPTOR_HEAP_TYPE / D3D12_HEAP_TYPE /
 *                 D3D12_COMMAND_LIST_TYPE / D3D12_RESOURCE_BARRIER_TYPE:
 *                 .../d3d12/ne-d3d12-*（d3d12.h）
 *            b) 本机 vendored 真实 SDK 头逐值比对（bgfx 3rdparty/dxsdk 的
 *               d3d12.h / dxgiformat.h，与最新 SDK ABI 值冻结一致）。
 *          重校结论：DXGI_FORMAT / RESOURCE_STATES / DIMENSION / ADDRESS /
 *          COMPARISON / BLEND / BLEND_OP / HEAP_TYPE / DESCRIPTOR_HEAP_TYPE /
 *          COMMAND_LIST_TYPE 各段原值全部正确；D3D12_FILTER 与
 *          D3D12_RESOURCE_BARRIER_TYPE 两段为直觉顺序误记，已按位编码真值重写。
 *
 *          注意：Enhanced Barriers（D3D12_BARRIER_SYNC/ACCESS）的位值在公开
 *          文档页未列数值，M0 不手定义——翻译表（D3D12StateMapping）落在
 *          经典 RESOURCE_STATES 域，M5+ 引入 Enhanced Barriers 时随
 *          PRIMAL_D3D12_ABI_CHECKS 一并锁值（见 D3D12StateMapping.h 的 TODO）。
 */

#pragma once

#include "Common/PrimitiveTypes.h"

namespace primal::graphics::rhi::d3d12 {

// ============================================================================
// DXGI_FORMAT（值 = dxgiformat.h 公开 ABI；仅收录 RHI DataFormat 映射所需 +
// TYPELESS 家族 + 常用邻居，不追求全集）
// ============================================================================
enum : u32 {
    kDXGIFormat_Unknown                    = 0,

    // 128-bit
    kDXGIFormat_R32G32B32A32_Typeless      = 1,
    kDXGIFormat_R32G32B32A32_Float         = 2,
    kDXGIFormat_R32G32B32A32_UInt          = 3,
    kDXGIFormat_R32G32B32A32_SInt          = 4,

    // 96-bit
    kDXGIFormat_R32G32B32_Typeless         = 5,
    kDXGIFormat_R32G32B32_Float            = 6,
    kDXGIFormat_R32G32B32_UInt             = 7,
    kDXGIFormat_R32G32B32_SInt             = 8,

    // 64-bit RGBA16
    kDXGIFormat_R16G16B16A16_Typeless      = 9,
    kDXGIFormat_R16G16B16A16_Float         = 10,
    kDXGIFormat_R16G16B16A16_UNorm         = 11,
    kDXGIFormat_R16G16B16A16_UInt          = 12,
    kDXGIFormat_R16G16B16A16_SNorm         = 13,
    kDXGIFormat_R16G16B16A16_SInt          = 14,

    // 64-bit RG32
    kDXGIFormat_R32G32_Typeless            = 15,
    kDXGIFormat_R32G32_Float               = 16,
    kDXGIFormat_R32G32_UInt                = 17,
    kDXGIFormat_R32G32_SInt                = 18,

    // 64-bit 深度模板（D32_FLOAT_S8X24 家族）
    kDXGIFormat_R32G8X24_Typeless          = 19,
    kDXGIFormat_D32_Float_S8X24_UInt       = 20,
    kDXGIFormat_R32_Float_X8X24_Typeless   = 21,
    kDXGIFormat_X32_Typeless_G8X24_UInt    = 22,

    // 32-bit packed + RGBA8
    kDXGIFormat_R10G10B10A2_Typeless       = 23,
    kDXGIFormat_R10G10B10A2_UNorm          = 24,
    kDXGIFormat_R10G10B10A2_UInt           = 25,
    kDXGIFormat_R11G11B10_Float            = 26,
    kDXGIFormat_R8G8B8A8_Typeless          = 27,
    kDXGIFormat_R8G8B8A8_UNorm             = 28,
    kDXGIFormat_R8G8B8A8_UNorm_sRGB        = 29,
    kDXGIFormat_R8G8B8A8_UInt              = 30,
    kDXGIFormat_R8G8B8A8_SNorm             = 31,
    kDXGIFormat_R8G8B8A8_SInt              = 32,

    // 32-bit RG16
    kDXGIFormat_R16G16_Typeless            = 33,
    kDXGIFormat_R16G16_Float               = 34,
    kDXGIFormat_R16G16_UNorm               = 35,
    kDXGIFormat_R16G16_UInt                = 36,
    kDXGIFormat_R16G16_SNorm               = 37,
    kDXGIFormat_R16G16_SInt                = 38,

    // 32-bit R32 / D24S8
    kDXGIFormat_R32_Typeless               = 39,
    kDXGIFormat_D32_Float                  = 40,
    kDXGIFormat_R32_Float                  = 41,
    kDXGIFormat_R32_UInt                   = 42,
    kDXGIFormat_R32_SInt                   = 43,
    kDXGIFormat_R24G8_Typeless             = 44,
    kDXGIFormat_D24_UNorm_S8_UInt          = 45,
    kDXGIFormat_R24_UNorm_X8_Typeless      = 46,
    kDXGIFormat_X24_Typeless_G8_UInt       = 47,

    // 16-bit RG8 / R16
    kDXGIFormat_R8G8_Typeless              = 48,
    kDXGIFormat_R8G8_UNorm                 = 49,
    kDXGIFormat_R8G8_UInt                  = 50,
    kDXGIFormat_R8G8_SNorm                 = 51,
    kDXGIFormat_R8G8_SInt                  = 52,
    kDXGIFormat_R16_Typeless               = 53,
    kDXGIFormat_R16_Float                  = 54,
    kDXGIFormat_D16_UNorm                  = 55,
    kDXGIFormat_R16_UNorm                  = 56,
    kDXGIFormat_R16_UInt                   = 57,
    kDXGIFormat_R16_SNorm                  = 58,
    kDXGIFormat_R16_SInt                   = 59,

    // 8-bit R8
    kDXGIFormat_R8_Typeless                = 60,
    kDXGIFormat_R8_UNorm                   = 61,
    kDXGIFormat_R8_UInt                    = 62,
    kDXGIFormat_R8_SNorm                   = 63,
    kDXGIFormat_R8_SInt                    = 64,

    // BC 压缩
    kDXGIFormat_BC1_Typeless               = 70,
    kDXGIFormat_BC1_UNorm                  = 71,
    kDXGIFormat_BC1_UNorm_sRGB             = 72,
    kDXGIFormat_BC2_Typeless               = 73,
    kDXGIFormat_BC2_UNorm                  = 74,
    kDXGIFormat_BC2_UNorm_sRGB             = 75,
    kDXGIFormat_BC3_Typeless               = 76,
    kDXGIFormat_BC3_UNorm                  = 77,
    kDXGIFormat_BC3_UNorm_sRGB             = 78,
    kDXGIFormat_BC4_Typeless               = 79,
    kDXGIFormat_BC4_UNorm                  = 80,
    kDXGIFormat_BC4_SNorm                  = 81,
    kDXGIFormat_BC5_Typeless               = 82,
    kDXGIFormat_BC5_UNorm                  = 83,
    kDXGIFormat_BC5_SNorm                  = 84,

    // BGRA8 家族（注意：DXGI 只有 UNORM / UNORM_SRGB / TYPELESS，无 SNORM/UInt/SInt）
    kDXGIFormat_B8G8R8A8_UNorm             = 87,
    kDXGIFormat_B8G8R8X8_UNorm             = 88,
    kDXGIFormat_B8G8R8A8_Typeless          = 90,
    kDXGIFormat_B8G8R8A8_UNorm_sRGB        = 91,

    // BC6H / BC7
    kDXGIFormat_BC6H_Typeless              = 94,
    kDXGIFormat_BC6H_UF16                  = 95,
    kDXGIFormat_BC6H_SF16                  = 96,
    kDXGIFormat_BC7_Typeless               = 97,
    kDXGIFormat_BC7_UNorm                  = 98,
    kDXGIFormat_BC7_UNorm_sRGB             = 99,
};

// ============================================================================
// D3D12_RESOURCE_STATES（d3d12.h 公开 ABI；GENERIC_READ 为组合值 0xAC3）
// ============================================================================
enum : u32 {
    kD3D12ResourceState_Common                         = 0,
    kD3D12ResourceState_VertexAndConstantBuffer        = 0x1,
    kD3D12ResourceState_IndexBuffer                    = 0x2,
    kD3D12ResourceState_RenderTarget                   = 0x4,
    kD3D12ResourceState_UnorderedAccess                = 0x8,
    kD3D12ResourceState_DepthWrite                     = 0x10,
    kD3D12ResourceState_DepthRead                      = 0x20,
    kD3D12ResourceState_NonPixelShaderResource         = 0x40,
    kD3D12ResourceState_PixelShaderResource            = 0x80,
    kD3D12ResourceState_StreamOut                      = 0x100,
    kD3D12ResourceState_IndirectArgument               = 0x200,  // 与 PREDICATION 同值
    kD3D12ResourceState_Predication                    = 0x200,
    kD3D12ResourceState_CopyDest                       = 0x400,
    kD3D12ResourceState_CopySource                     = 0x800,
    kD3D12ResourceState_ResolveDest                    = 0x1000,
    kD3D12ResourceState_ResolveSource                  = 0x2000,
    kD3D12ResourceState_VideoDecodeRead                = 0x10000,
    kD3D12ResourceState_VideoDecodeWrite               = 0x20000,
    kD3D12ResourceState_VideoProcessRead               = 0x40000,
    kD3D12ResourceState_VideoProcessWrite              = 0x80000,
    kD3D12ResourceState_VideoEncodeRead                = 0x200000,
    kD3D12ResourceState_VideoEncodeWrite               = 0x800000,
    kD3D12ResourceState_RaytracingAccelerationStructure = 0x400000,
    kD3D12ResourceState_ShadingRateSource              = 0x1000000,
    kD3D12ResourceState_Present                        = 0,      // = COMMON 的同义词

    // 组合便利值（d3d12.h 原样定义的组合）
    kD3D12ResourceState_AllShaderResource              = kD3D12ResourceState_NonPixelShaderResource
                                                       | kD3D12ResourceState_PixelShaderResource,  // 0xC0
    kD3D12ResourceState_GenericRead                    = kD3D12ResourceState_VertexAndConstantBuffer
                                                       | kD3D12ResourceState_IndexBuffer
                                                       | kD3D12ResourceState_NonPixelShaderResource
                                                       | kD3D12ResourceState_PixelShaderResource
                                                       | kD3D12ResourceState_IndirectArgument
                                                       | kD3D12ResourceState_CopySource,           // 0xAC3
};

// ============================================================================
// D3D12_RESOURCE_DIMENSION
// ============================================================================
enum : u32 {
    kD3D12ResourceDimension_Unknown    = 0,
    kD3D12ResourceDimension_Buffer     = 1,
    kD3D12ResourceDimension_Texture1D  = 2,
    kD3D12ResourceDimension_Texture2D  = 3,
    kD3D12ResourceDimension_Texture3D  = 4,
};

// ============================================================================
// D3D12_FILTER —— 位编码，非顺序编号（2026-11-03 全表重校）：
//   bits[5:4]=min 过滤（0=point,1=linear）  bits[3:2]=mag  bits[1:0]=mip
//   bit[6]=各向异性标志（0x40，须 min/mag/mip 全 linear）
//   bits[8:7]=reduction（0=标准,1=comparison(+0x80),2=minimum(+0x100),
//                          3=maximum(+0x180)）
// 因此：基础 8 档 = 0x0/0x1/0x4/0x5/0x10/0x11/0x14/0x15（trilinear =
// MIN_MAG_MIP_LINEAR = 0x15，不是直觉顺序的 7，也不是 0x41）；ANISOTROPIC =
// 0x55；COMPARISON 族 = 基础档 + 0x80（0x80..0x95、0xd5）。
// 注意 SDK 成员命名（d3d12.h / Microsoft Learn D3D12_FILTER 页）：
//   0x5  = D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR（无 "MIN_POINT_MAG_LINEAR_
//          MIP_LINEAR" 这一成员名）
//   0x85 = D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR
//   0x15 = D3D12_FILTER_MIN_MAG_MIP_LINEAR；0x14 = MIN_MAG_LINEAR_MIP_POINT
// 本表沿用仓库自有的全拼命名（MinPointMagLinearMipLinear 等），语义与上述
// SDK 档一一对应；AbiChecks.cpp 按真实 SDK 成员名锁值。
// Minimum/Maximum 变体（+0x100/+0x180）本仓 RHI 无对应 FilterMode，不定义。
// ============================================================================
enum : u32 {
    kD3D12Filter_MinMagMipPoint                        = 0,
    kD3D12Filter_MinMagPointMipLinear                  = 0x1,
    kD3D12Filter_MinPointMagLinearMipPoint             = 0x4,
    kD3D12Filter_MinPointMagLinearMipLinear            = 0x5,   // SDK: MIN_POINT_MAG_MIP_LINEAR
    kD3D12Filter_MinLinearMagMipPoint                  = 0x10,
    kD3D12Filter_MinLinearMagPointMipLinear            = 0x11,
    kD3D12Filter_MinMagLinearMipPoint                  = 0x14,
    kD3D12Filter_MinMagMipLinear                       = 0x15,  // trilinear
    kD3D12Filter_Anisotropic                           = 0x55,
    kD3D12Filter_ComparisonMinMagMipPoint              = 0x80,
    kD3D12Filter_ComparisonMinMagPointMipLinear        = 0x81,
    kD3D12Filter_ComparisonMinPointMagLinearMipPoint   = 0x84,
    kD3D12Filter_ComparisonMinPointMagLinearMipLinear  = 0x85,  // SDK: COMPARISON_MIN_POINT_MAG_MIP_LINEAR
    kD3D12Filter_ComparisonMinLinearMagMipPoint        = 0x90,
    kD3D12Filter_ComparisonMinLinearMagPointMipLinear  = 0x91,
    kD3D12Filter_ComparisonMinMagLinearMipPoint        = 0x94,
    kD3D12Filter_ComparisonMinMagMipLinear             = 0x95,
    kD3D12Filter_ComparisonAnisotropic                 = 0xd5,
};

// ============================================================================
// D3D12_TEXTURE_ADDRESS_MODE（注意从 1 开始，0 不是合法值）
// ============================================================================
enum : u32 {
    kD3D12TextureAddressMode_Wrap        = 1,
    kD3D12TextureAddressMode_Mirror      = 2,
    kD3D12TextureAddressMode_Clamp       = 3,
    kD3D12TextureAddressMode_Border      = 4,
    kD3D12TextureAddressMode_MirrorOnce  = 5,
};

// ============================================================================
// D3D12_COMPARISON_FUNC（从 1 开始）
// ============================================================================
enum : u32 {
    kD3D12ComparisonFunc_Never         = 1,
    kD3D12ComparisonFunc_Less          = 2,
    kD3D12ComparisonFunc_Equal         = 3,
    kD3D12ComparisonFunc_LessEqual     = 4,
    kD3D12ComparisonFunc_Greater       = 5,
    kD3D12ComparisonFunc_NotEqual      = 6,
    kD3D12ComparisonFunc_GreaterEqual  = 7,
    kD3D12ComparisonFunc_Always        = 8,
};

// ============================================================================
// D3D12_BLEND（从 1 开始；12/13 是历史保留空洞）
// ============================================================================
enum : u32 {
    kD3D12Blend_Zero            = 1,
    kD3D12Blend_One             = 2,
    kD3D12Blend_SrcColor        = 3,
    kD3D12Blend_InvSrcColor     = 4,
    kD3D12Blend_SrcAlpha        = 5,
    kD3D12Blend_InvSrcAlpha     = 6,
    kD3D12Blend_DestAlpha       = 7,
    kD3D12Blend_InvDestAlpha    = 8,
    kD3D12Blend_DestColor       = 9,
    kD3D12Blend_InvDestColor    = 10,
    kD3D12Blend_SrcAlphaSat     = 11,
    // 12/13 保留
    kD3D12Blend_BlendFactor     = 14,
    kD3D12Blend_InvBlendFactor  = 15,
    kD3D12Blend_Src1Color       = 16,
    kD3D12Blend_InvSrc1Color    = 17,
    kD3D12Blend_Src1Alpha       = 18,
    kD3D12Blend_InvSrc1Alpha    = 19,
};

// ============================================================================
// D3D12_BLEND_OP
// ============================================================================
enum : u32 {
    kD3D12BlendOp_Add          = 1,
    kD3D12BlendOp_Subtract     = 2,
    kD3D12BlendOp_RevSubtract  = 3,
    kD3D12BlendOp_Min          = 4,
    kD3D12BlendOp_Max          = 5,
};

// ============================================================================
// D3D12_DESCRIPTOR_HEAP_TYPE
// ============================================================================
enum : u32 {
    kD3D12DescriptorHeapType_CBV_SRV_UAV = 0,
    kD3D12DescriptorHeapType_Sampler     = 1,
    kD3D12DescriptorHeapType_RTV         = 2,
    kD3D12DescriptorHeapType_DSV         = 3,
    kD3D12DescriptorHeapType_NumTypes    = 4,
};

// ============================================================================
// D3D12_HEAP_TYPE（F0-8 设备骨架创建 committed resource 用）
// ============================================================================
enum : u32 {
    kD3D12HeapType_Default = 1,
    kD3D12HeapType_Upload  = 2,
    kD3D12HeapType_Readback = 3,
    kD3D12HeapType_Custom  = 4,
};

// ============================================================================
// D3D12_COMMAND_LIST_TYPE（F0-8 DIRECT queue 骨架用）
// ============================================================================
enum : u32 {
    kD3D12CommandListType_Direct = 0,
    kD3D12CommandListType_Bundle = 1,
    kD3D12CommandListType_Compute = 2,
    kD3D12CommandListType_Copy = 3,
};

// ============================================================================
// 经典 Resource Barrier 类型（D3D12_RESOURCE_BARRIER_TYPE；供
// D3D12StateTracker 的 op 序列语义对齐，Windows 层映射到真实结构）。
// 2026-11-03 重校：d3d12.h 仅有三个成员 TRANSITION=0 / ALIASING=1 / UAV=2
// （Microsoft Learn D3D12_RESOURCE_BARRIER_TYPE 页 + 本机 vendored dxsdk
// 头双源一致）。旧表的 RTV=1 / Aliasing=3 是误记：D3D12 无 RTV barrier
// 类型（render-target 依赖走 TRANSITION），Aliasing 真值=1。
// ============================================================================
enum : u32 {
    kD3D12ResourceBarrierType_Transition = 0,
    kD3D12ResourceBarrierType_Aliasing = 1,
    kD3D12ResourceBarrierType_UAV = 2,
};

/// D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES（Subresource 字段的全子资源哨兵）
constexpr u32 kD3D12ResourceBarrierAllSubresources = 0xFFFFFFFFu;

/// Root signature 硬上限（d3d12.h：MaxRootSignatureDWORDs = 64）
constexpr u32 kD3D12RootSignatureMaxDwords = 64;

} // namespace primal::graphics::rhi::d3d12

// ============================================================================
// PRIMAL_D3D12_ABI_CHECKS —— 已于 M7 F7-7 落为真实核对单元：
// 同目录 D3D12AbiChecks.cpp（WIN32+ENABLE_D3D12 门控，随 Engine 的 D3D12
// glob 编入双后端构建）对本文件全部常量逐一 static_assert 到真实
// <d3d12.h>/<dxgiformat.h>。常量漂移=Windows 构建编译失败（构建门本体）。
// 原注释模板由该文件取代（2026-11-03）。
// ============================================================================
