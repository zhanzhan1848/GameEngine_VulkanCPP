/**
 * @file D3D12StateMapping.cpp
 * @brief RHI <-> D3D12 状态翻译实现（平台中立）
 * @details 规则表驱动的断言见 EngineTest/UnitTests/RHI/D3D12/TestD3D12StateTracker.cpp
 *          （映射断言与跟踪器断言共用该文件）。
 */

#include "D3D12StateMapping.h"

namespace primal::graphics::rhi::d3d12 {

bool ToD3D12ResourceStates(ResourceState state, u32& bits) {
    switch (state) {
        // 布局态（RHITypes.h 注释即按 D3D12 语义命名）
        case ResourceState::General:              bits = kD3D12ResourceState_Common; break;
        case ResourceState::ShaderResource:       bits = kD3D12ResourceState_AllShaderResource; break;
        case ResourceState::RenderTarget:         bits = kD3D12ResourceState_RenderTarget; break;
        case ResourceState::DepthStencil:         bits = kD3D12ResourceState_DepthWrite; break;
        case ResourceState::DepthStencilReadOnly: bits = kD3D12ResourceState_DepthRead; break;
        case ResourceState::UnorderedAccess:      bits = kD3D12ResourceState_UnorderedAccess; break;
        case ResourceState::CopySource:           bits = kD3D12ResourceState_CopySource; break;
        case ResourceState::CopyDest:             bits = kD3D12ResourceState_CopyDest; break;
        case ResourceState::Present:              bits = kD3D12ResourceState_Present; break;  // = COMMON
        case ResourceState::ResolveSource:        bits = kD3D12ResourceState_ResolveSource; break;
        case ResourceState::ResolveDest:          bits = kD3D12ResourceState_ResolveDest; break;
        case ResourceState::IndirectArgument:     bits = kD3D12ResourceState_IndirectArgument; break;
        default:
            // 生命周期态（Unknown/Created/Allocated/PendingUpload/Ready/
            // InUse/PendingDestroy/Destroyed）无 D3D12 等价物。
            return false;
    }
    return true;
}

u32 ToD3D12ResourceStatesOrCommon(ResourceState state) {
    u32 bits = kD3D12ResourceState_Common;
    ToD3D12ResourceStates(state, bits);
    return bits;
}

ResourceState FromD3D12ResourceStates(u32 bits) {
    switch (bits) {
        case kD3D12ResourceState_Common:             return ResourceState::General;
        case kD3D12ResourceState_AllShaderResource:  return ResourceState::ShaderResource;
        case kD3D12ResourceState_RenderTarget:       return ResourceState::RenderTarget;
        case kD3D12ResourceState_DepthWrite:         return ResourceState::DepthStencil;
        case kD3D12ResourceState_DepthRead:          return ResourceState::DepthStencilReadOnly;
        case kD3D12ResourceState_UnorderedAccess:    return ResourceState::UnorderedAccess;
        case kD3D12ResourceState_CopySource:         return ResourceState::CopySource;
        case kD3D12ResourceState_CopyDest:           return ResourceState::CopyDest;
        case kD3D12ResourceState_ResolveSource:      return ResourceState::ResolveSource;
        case kD3D12ResourceState_ResolveDest:        return ResourceState::ResolveDest;
        case kD3D12ResourceState_IndirectArgument:   return ResourceState::IndirectArgument;
        default:
            return ResourceState::Unknown;
    }
}

u32 PipelineStageToResourceStatesBits(PipelineStage stage) {
    u32 bits = 0;
    const u32 s = static_cast<u32>(stage);
    if (s & static_cast<u32>(PipelineStage::DrawIndirect))
        bits |= kD3D12ResourceState_IndirectArgument;
    if (s & static_cast<u32>(PipelineStage::VertexInput))
        bits |= kD3D12ResourceState_VertexAndConstantBuffer | kD3D12ResourceState_IndexBuffer;
    // 注意：RHI PipelineStage 无 Geometry/Hull/Domain 位（那是 ShaderStage 的
    // 成员）；非像素图元阶段由 VertexShader 位代表。
    if (s & static_cast<u32>(PipelineStage::VertexShader))
        bits |= kD3D12ResourceState_NonPixelShaderResource;
    if (s & static_cast<u32>(PipelineStage::FragmentShader))
        bits |= kD3D12ResourceState_PixelShaderResource;
    if (s & (static_cast<u32>(PipelineStage::EarlyFragmentTests)
             | static_cast<u32>(PipelineStage::LateFragmentTests)))
        bits |= kD3D12ResourceState_DepthRead;
    if (s & static_cast<u32>(PipelineStage::ColorAttachmentOutput))
        bits |= kD3D12ResourceState_RenderTarget;
    if (s & static_cast<u32>(PipelineStage::ComputeShader))
        bits |= kD3D12ResourceState_NonPixelShaderResource;
    if (s & static_cast<u32>(PipelineStage::Transfer))
        bits |= kD3D12ResourceState_CopySource | kD3D12ResourceState_CopyDest;
    // TopOfPipe/BottomOfPipe/Host/AllGraphics/AllCommands：无单一资源状态位
    // 等价（对应全同步域），bits 保持已累积部分。
    return bits;
}

u32 AccessFlagsToResourceStatesBits(AccessFlag access) {
    u32 bits = 0;
    const u32 a = static_cast<u32>(access);
    if (a & static_cast<u32>(AccessFlag::IndirectCommandRead))
        bits |= kD3D12ResourceState_IndirectArgument;
    if (a & static_cast<u32>(AccessFlag::IndexRead))
        bits |= kD3D12ResourceState_IndexBuffer;
    if (a & static_cast<u32>(AccessFlag::VertexAttributeRead))
        bits |= kD3D12ResourceState_VertexAndConstantBuffer;
    if (a & static_cast<u32>(AccessFlag::UniformRead))
        bits |= kD3D12ResourceState_VertexAndConstantBuffer;
    if (a & static_cast<u32>(AccessFlag::InputAttachmentRead))
        bits |= kD3D12ResourceState_PixelShaderResource;
    if (a & static_cast<u32>(AccessFlag::ShaderRead)) {
        // SRV 读；若同时有 ShaderWrite 则合并为 UAV（下方分支补齐）
        bits |= kD3D12ResourceState_AllShaderResource;
    }
    if (a & static_cast<u32>(AccessFlag::ShaderWrite)) {
        // 写目标按 UAV（compute 写镜像 rhi-barrier-semantics.md §1 约定 1）
        bits |= kD3D12ResourceState_UnorderedAccess;
    }
    if (a & static_cast<u32>(AccessFlag::ColorAttachmentRead))
        bits |= kD3D12ResourceState_RenderTarget;
    if (a & static_cast<u32>(AccessFlag::ColorAttachmentWrite))
        bits |= kD3D12ResourceState_RenderTarget;
    if (a & static_cast<u32>(AccessFlag::DepthStencilAttachmentRead))
        bits |= kD3D12ResourceState_DepthRead;
    if (a & static_cast<u32>(AccessFlag::DepthStencilAttachmentWrite))
        bits |= kD3D12ResourceState_DepthWrite;
    if (a & static_cast<u32>(AccessFlag::TransferRead))
        bits |= kD3D12ResourceState_CopySource;
    if (a & static_cast<u32>(AccessFlag::TransferWrite))
        bits |= kD3D12ResourceState_CopyDest;
    // HostRead/HostWrite/MemoryRead/MemoryWrite：对应 COMMON/上传堆路径，
    // 无独立资源状态位。
    return bits;
}

bool IsReadClassState(ResourceState state) {
    switch (state) {
        case ResourceState::ShaderResource:
        case ResourceState::CopySource:
        case ResourceState::IndirectArgument:
            return true;
        default:
            return false;
    }
}

u32 ToD3D12ComparisonFunc(ComparisonFunc func) {
    switch (func) {
        case ComparisonFunc::Never:        return kD3D12ComparisonFunc_Never;
        case ComparisonFunc::Less:         return kD3D12ComparisonFunc_Less;
        case ComparisonFunc::Equal:        return kD3D12ComparisonFunc_Equal;
        case ComparisonFunc::LessEqual:    return kD3D12ComparisonFunc_LessEqual;
        case ComparisonFunc::Greater:      return kD3D12ComparisonFunc_Greater;
        case ComparisonFunc::NotEqual:     return kD3D12ComparisonFunc_NotEqual;
        case ComparisonFunc::GreaterEqual: return kD3D12ComparisonFunc_GreaterEqual;
        case ComparisonFunc::Always:       return kD3D12ComparisonFunc_Always;
        default:                           return kD3D12ComparisonFunc_Never;  // Unknown → 最保守
    }
}

bool SamplerDescWantsComparison(const SamplerDesc& desc) {
    // 与 VulkanSampler::Initialize 的 compareEnable 谓词逐字对齐
    // （comparisonFunc != Never）。见头文件注：M1 版
    // "!= Always && != Unknown" 谓词与 Vulkan 在 Never/Always 两值上
    // 各有一处口径分歧，M2 F2-3 修正为单端谓词复刻。
    return desc.comparisonFunc != ComparisonFunc::Never;
}

u32 SelectD3D12SamplerFilter(const SamplerDesc& desc) {
    // 比较采样器物理上要求 COMPARISON_* 滤波枚举（SampleCmp 专位）。
    // 滤波档位（点/线性组合 + 各向异性）与非比较档一一对应，比较谓词
    // 满足时平移到 COMPARISON 变体，不静默改变档位。
    if (SamplerDescWantsComparison(desc)) {
        if (desc.maxAnisotropy > 1) return kD3D12Filter_ComparisonAnisotropic;
    } else if (desc.maxAnisotropy > 1) {
        return kD3D12Filter_Anisotropic;
    }
    const bool minLinear = desc.minFilter != FilterMode::Point;
    const bool magLinear = desc.magFilter != FilterMode::Point;
    const bool mipLinear = desc.mipFilter != FilterMode::Point;
    // 三布尔八档平铺 switch（与 D3D12Abi.h 滤波表逐值对照）。
    u32 base;
    if (!minLinear && !magLinear && !mipLinear)      base = kD3D12Filter_MinMagMipPoint;           // 0x00
    else if (!minLinear && !magLinear && mipLinear)  base = kD3D12Filter_MinMagPointMipLinear;     // 0x01
    else if (!minLinear && magLinear && !mipLinear)  base = kD3D12Filter_MinPointMagLinearMipPoint;// 0x02
    else if (!minLinear && magLinear && mipLinear)   base = kD3D12Filter_MinPointMagLinearMipLinear;// 0x03
    else if (minLinear && !magLinear && !mipLinear)  base = kD3D12Filter_MinLinearMagMipPoint;     // 0x04
    else if (minLinear && !magLinear && mipLinear)   base = kD3D12Filter_MinLinearMagPointMipLinear;// 0x05
    else if (minLinear && magLinear && !mipLinear)   base = kD3D12Filter_MinMagLinearMipPoint;     // 0x06
    else                                              base = kD3D12Filter_MinMagMipLinear;          // 0x07
    // COMPARISON 变体恰好是 base + 0x10（D3D12_FILTER 枚举布局），
    // D3D12Abi.h 已按真实枚举锁值；不依赖该算术关系，显式平移：
    if (!SamplerDescWantsComparison(desc)) return base;
    switch (base) {
        case kD3D12Filter_MinMagMipPoint:           return kD3D12Filter_ComparisonMinMagMipPoint;
        case kD3D12Filter_MinMagPointMipLinear:     return kD3D12Filter_ComparisonMinMagPointMipLinear;
        case kD3D12Filter_MinPointMagLinearMipPoint:return kD3D12Filter_ComparisonMinPointMagLinearMipPoint;
        case kD3D12Filter_MinPointMagLinearMipLinear:return kD3D12Filter_ComparisonMinPointMagLinearMipLinear;
        case kD3D12Filter_MinLinearMagMipPoint:     return kD3D12Filter_ComparisonMinLinearMagMipPoint;
        case kD3D12Filter_MinLinearMagPointMipLinear:return kD3D12Filter_ComparisonMinLinearMagPointMipLinear;
        case kD3D12Filter_MinMagLinearMipPoint:     return kD3D12Filter_ComparisonMinMagLinearMipPoint;
        default:                                    return kD3D12Filter_ComparisonMinMagMipLinear;
    }
}

} // namespace primal::graphics::rhi::d3d12
