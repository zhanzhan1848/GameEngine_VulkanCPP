/**
 * @file D3D12RootSignatureBudget.h
 * @brief Root signature 64-DWORD 预算器（平台中立，无 Windows 头）
 * @details D3D12 RHI 现代化 M0（F0-5 预算器部分）。
 *
 *          D3D12 root signature 硬上限 64 DWORD（kD3D12RootSignatureMaxDwords）。
 *          每类 root parameter 占用：
 *            - root descriptor（内联 CBV/SRV/UAV）= 2 DWORD
 *            - descriptor table（指向堆的一段）  = 1 DWORD
 *            - root constants                     = ceil(count32Bit / 4) DWORD
 *          超限 = 显式失败（ok=false + 原因），绝不静默截断。
 *
 *          RHI DescriptorSetLayoutDesc -> root 参数的换算约定（M1 Pipeline
 *          落地前冻结）：每个 descriptor set 映射为一个 descriptor table
 *          （1 DWORD）；RHI push-constant 尺寸按 root constants 换算。
 */

#pragma once

#include "../../Core/RHITypes.h"
#include "D3D12Abi.h"

namespace primal::graphics::rhi::d3d12 {

/// Root parameter 种类（预算相关的最小面）。
struct D3D12RootParameterDesc {
    enum class Kind : u8 {
        DescriptorTable,  ///< 指向描述符堆区段（1 DWORD）
        Constants32Bit,   ///< root constants（ceil(count/4) DWORD）
        RootCBV,          ///< 内联常量缓冲视图（2 DWORD）
        RootSRV,          ///< 内联着色器资源视图（2 DWORD）
        RootUAV,          ///< 内联无序访问视图（2 DWORD）
    };

    Kind kind = Kind::DescriptorTable;
    u32 constantCount = 0;     ///< Constants32Bit：32 位常量个数
    u32 descriptorCount = 0;   ///< DescriptorTable：表内描述符总数（诊断用，
                               ///< 不影响 DWORD 预算——表本身恒 1 DWORD）
};

/// 预算结果。ok=false 时 reason 给出首个失败原因。
struct D3D12RootSignatureBudget {
    u32 dwords = 0;            ///< 总占用（含静态采样器为 0——D3D12 静态
                               ///< 采样器不占 root space）
    u32 parameterCount = 0;    ///< root parameter 个数
    u32 descriptorCount = 0;   ///< 表内描述符总数（诊断）
    bool ok = false;           ///< 是否在 64-DWORD 上限内
    const char* reason = "";   ///< 失败原因（""=成功）
};

/// 单个 root parameter 的 DWORD 占用。
u32 RootParameterCostDwords(const D3D12RootParameterDesc& param);

/// 预算完整 root signature（参数顺序即 root signature 声明顺序）。
/// 超过 kD3D12RootSignatureMaxDwords 显式失败（ok=false）。
D3D12RootSignatureBudget EvaluateRootSignature(const D3D12RootParameterDesc* params,
                                               u32 paramCount);

/// 便捷重载。
D3D12RootSignatureBudget EvaluateRootSignature(const utl::vector<D3D12RootParameterDesc>& params);

/// RHI 布局换算：每个 DescriptorSetLayoutDesc 映射为一个 descriptor table
/// （1 DWORD/表；descriptorCount 累计仅作诊断）。pushConstantSizeBytes>0 时
/// 追加一个 Constants32Bit 参数（ceil(bytes/4) 个 32 位常量）。
/// 与 M1 管线实现的映射约定保持一致（见文件头注释）。
D3D12RootSignatureBudget EstimateRootSignatureForLayouts(
        const DescriptorSetLayoutDesc* layouts, u32 layoutCount,
        u32 pushConstantSizeBytes = 0);

} // namespace primal::graphics::rhi::d3d12
