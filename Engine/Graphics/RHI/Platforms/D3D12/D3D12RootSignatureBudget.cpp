/**
 * @file D3D12RootSignatureBudget.cpp
 * @brief Root signature 64-DWORD 预算器实现（平台中立）
 * @details 计数断言见 EngineTest/UnitTests/RHI/D3D12/TestD3D12RootSigBudget.cpp。
 */

#include "D3D12RootSignatureBudget.h"

namespace primal::graphics::rhi::d3d12 {

u32 RootParameterCostDwords(const D3D12RootParameterDesc& param) {
    switch (param.kind) {
        case D3D12RootParameterDesc::Kind::DescriptorTable:
            return 1;  // 表指针恒 1 DWORD（表内容不占 root space）
        case D3D12RootParameterDesc::Kind::Constants32Bit: {
            // root constants 按 32 位值计，4 个 = 1 DWORD，向上取整。
            const u32 count = param.constantCount;
            return (count + 3u) / 4u;
        }
        case D3D12RootParameterDesc::Kind::RootCBV:
        case D3D12RootParameterDesc::Kind::RootSRV:
        case D3D12RootParameterDesc::Kind::RootUAV:
            return 2;  // 64-bit GPU 虚拟地址
        default:
            return 0;
    }
}

D3D12RootSignatureBudget EvaluateRootSignature(const D3D12RootParameterDesc* params,
                                               u32 paramCount) {
    D3D12RootSignatureBudget budget;
    if (!params && paramCount > 0) {
        budget.reason = "null_params";
        return budget;
    }
    budget.parameterCount = paramCount;
    u64 total = 0;  // 64 位累加防溢出再判上限
    for (u32 i = 0; i < paramCount; ++i) {
        const u32 cost = RootParameterCostDwords(params[i]);
        total += cost;
        budget.descriptorCount += params[i].descriptorCount;
        if (total > kD3D12RootSignatureMaxDwords) {
            budget.dwords = u32(total);  // 超限值照报（诊断）
            budget.reason = "exceeds_64_dword_limit";
            return budget;
        }
    }
    budget.dwords = u32(total);
    budget.ok = true;
    budget.reason = "";
    return budget;
}

D3D12RootSignatureBudget EvaluateRootSignature(const utl::vector<D3D12RootParameterDesc>& params) {
    return EvaluateRootSignature(params.data(), static_cast<u32>(params.size()));
}

D3D12RootSignatureBudget EstimateRootSignatureForLayouts(
        const DescriptorSetLayoutDesc* layouts, u32 layoutCount,
        u32 pushConstantSizeBytes) {
    utl::vector<D3D12RootParameterDesc> params;
    params.reserve(layoutCount + (pushConstantSizeBytes ? 1u : 0u));
    for (u32 i = 0; i < layoutCount; ++i) {
        D3D12RootParameterDesc table;
        table.kind = D3D12RootParameterDesc::Kind::DescriptorTable;
        u32 descriptors = 0;
        for (u32 b = 0; b < layouts[i].bindingCount; ++b) {
            const auto& binding = layouts[i].bindings[b];
            // CombinedImageSampler 占两个堆位（SRV+SAMPLER），其余一描述符。
            descriptors += binding.descriptorType == DescriptorType::CombinedImageSampler
                ? (binding.descriptorCount ? binding.descriptorCount * 2u : 2u)
                : binding.descriptorCount;
        }
        table.descriptorCount = descriptors;
        params.push_back(table);
    }
    if (pushConstantSizeBytes) {
        D3D12RootParameterDesc constants;
        constants.kind = D3D12RootParameterDesc::Kind::Constants32Bit;
        constants.constantCount = (pushConstantSizeBytes + 3u) / 4u;
        params.push_back(constants);
    }
    return EvaluateRootSignature(params);
}

} // namespace primal::graphics::rhi::d3d12
