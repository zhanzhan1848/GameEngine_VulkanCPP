// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Pipeline.cpp — PSO + root signature 实现（M1 F1-4）。
#include "D3D12Pipeline.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <algorithm>
#include <cstring>

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Format.h"
#include "D3D12RootSignatureBudget.h"
#include "D3D12Shader.h"

#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi::d3d12 {

// ============================================================================
// 枚举直译（全部经 D3D12Abi 常量，不写裸值）
// ============================================================================

namespace {

D3D12_COMPARISON_FUNC ToComparisonFunc(ComparisonFunc func) {
    switch (func) {
        case ComparisonFunc::Never:        return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Never);
        case ComparisonFunc::Less:         return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Less);
        case ComparisonFunc::Equal:        return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Equal);
        case ComparisonFunc::LessEqual:    return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_LessEqual);
        case ComparisonFunc::Greater:      return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Greater);
        case ComparisonFunc::NotEqual:     return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_NotEqual);
        case ComparisonFunc::GreaterEqual: return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_GreaterEqual);
        case ComparisonFunc::Always:       return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Always);
        default:                           return static_cast<D3D12_COMPARISON_FUNC>(kD3D12ComparisonFunc_Always);
    }
}

D3D12_BLEND ToBlend(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:           return static_cast<D3D12_BLEND>(kD3D12Blend_Zero);
        case BlendFactor::One:            return static_cast<D3D12_BLEND>(kD3D12Blend_One);
        case BlendFactor::SrcColor:       return static_cast<D3D12_BLEND>(kD3D12Blend_SrcColor);
        case BlendFactor::InvSrcColor:    return static_cast<D3D12_BLEND>(kD3D12Blend_InvSrcColor);
        case BlendFactor::SrcAlpha:       return static_cast<D3D12_BLEND>(kD3D12Blend_SrcAlpha);
        case BlendFactor::InvSrcAlpha:    return static_cast<D3D12_BLEND>(kD3D12Blend_InvSrcAlpha);
        case BlendFactor::DestAlpha:      return static_cast<D3D12_BLEND>(kD3D12Blend_DestAlpha);
        case BlendFactor::InvDestAlpha:   return static_cast<D3D12_BLEND>(kD3D12Blend_InvDestAlpha);
        case BlendFactor::DestColor:      return static_cast<D3D12_BLEND>(kD3D12Blend_DestColor);
        case BlendFactor::InvDestColor:   return static_cast<D3D12_BLEND>(kD3D12Blend_InvDestColor);
        case BlendFactor::SrcAlphaSat:    return static_cast<D3D12_BLEND>(kD3D12Blend_SrcAlphaSat);
        case BlendFactor::BlendFactor:    return static_cast<D3D12_BLEND>(kD3D12Blend_BlendFactor);
        case BlendFactor::InvBlendFactor: return static_cast<D3D12_BLEND>(kD3D12Blend_InvBlendFactor);
        case BlendFactor::Src1Color:      return static_cast<D3D12_BLEND>(kD3D12Blend_Src1Color);
        case BlendFactor::InvSrc1Color:   return static_cast<D3D12_BLEND>(kD3D12Blend_InvSrc1Color);
        case BlendFactor::Src1Alpha:      return static_cast<D3D12_BLEND>(kD3D12Blend_Src1Alpha);
        case BlendFactor::InvSrc1Alpha:   return static_cast<D3D12_BLEND>(kD3D12Blend_InvSrc1Alpha);
        default:                          return static_cast<D3D12_BLEND>(kD3D12Blend_One);
    }
}

D3D12_BLEND_OP ToBlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::Add:         return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_Add);
        case BlendOp::Subtract:    return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_Subtract);
        case BlendOp::RevSubtract: return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_RevSubtract);
        case BlendOp::Min:         return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_Min);
        case BlendOp::Max:         return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_Max);
        default:                   return static_cast<D3D12_BLEND_OP>(kD3D12BlendOp_Add);
    }
}

D3D12_STENCIL_OP ToStencilOp(StencilOp op) {
    switch (op) {
        case StencilOp::Keep:    return D3D12_STENCIL_OP_KEEP;
        case StencilOp::Zero:    return D3D12_STENCIL_OP_ZERO;
        case StencilOp::Replace: return D3D12_STENCIL_OP_REPLACE;
        case StencilOp::IncSat:  return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOp::DecSat:  return D3D12_STENCIL_OP_DECR_SAT;
        case StencilOp::Invert:  return D3D12_STENCIL_OP_INVERT;
        case StencilOp::Inc:     return D3D12_STENCIL_OP_INCR;
        case StencilOp::Dec:     return D3D12_STENCIL_OP_DECR;
        default:                 return D3D12_STENCIL_OP_KEEP;
    }
}

/// CullMode 直译契约（§7.2-1 修订，H2 修复）：负 viewport 高度（SetViewport）
/// 恰好补偿 NDC Y 语义差（Vulkan Y-down vs D3D12 Y-up），光栅化绕序与
/// Vulkan（frontFace=CW）一致；D3D12 默认 FrontCounterClockwise=FALSE 即
/// CW=正面，同语义 → cull 不取反（Front→FRONT，Back→BACK，None 不变）。
/// 历史版本在此再翻一次 cull（视口翻转+cull 翻转=双重取反）→ 绕序判定
/// 反转、正面被整体剔除（全黑帧根因 H2），已移除。
D3D12_CULL_MODE ToCullMode(CullMode mode) {
    switch (mode) {
        case CullMode::Front: return D3D12_CULL_MODE_FRONT;
        case CullMode::Back:  return D3D12_CULL_MODE_BACK;
        case CullMode::None:
        case CullMode::Unknown:
        default:              return D3D12_CULL_MODE_NONE;
    }
}

void ToTopology(PrimitiveTopology topology,
                D3D12_PRIMITIVE_TOPOLOGY_TYPE& outType,
                D3D_PRIMITIVE_TOPOLOGY& outIa) {
    switch (topology) {
        case PrimitiveTopology::PointList:
            outType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            outIa = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case PrimitiveTopology::LineList:
            outType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            outIa = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case PrimitiveTopology::LineStrip:
            outType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            outIa = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case PrimitiveTopology::TriangleStrip:
            outType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            outIa = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::Unknown:
        default:
            outType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            outIa = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        // Adj 变体与 PatchList（Hull/Domain）M1 语料未使用——登记 NOT-IMPLEMENTED
        // （到达到时落 D3D_PRIMITIVE_TOPOLOGY_*_ADJ / PATCHLIST + D3D12 TOPOLOGY_TYPE_PATCH）。
    }
}

D3D12_SHADER_VISIBILITY ToVisibility(ShaderStage stage) {
    // 单一阶段 → 对应 visibility；组合/Unknown → ALL（保守可见）。
    switch (stage) {
        case ShaderStage::Vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
        case ShaderStage::Pixel:  return D3D12_SHADER_VISIBILITY_PIXEL;
        case ShaderStage::Geometry: return D3D12_SHADER_VISIBILITY_GEOMETRY;
        case ShaderStage::Hull:   return D3D12_SHADER_VISIBILITY_HULL;
        case ShaderStage::Domain: return D3D12_SHADER_VISIBILITY_DOMAIN;
        case ShaderStage::Compute: return D3D12_SHADER_VISIBILITY_ALL;  // compute root sig 用 ALL
        default: return D3D12_SHADER_VISIBILITY_ALL;
    }
}

/// H1 仪表：根签名日志的 visibility/range 类型可读名（仅诊断用）。
const char* VisibilityName(D3D12_SHADER_VISIBILITY v) {
    switch (v) {
        case D3D12_SHADER_VISIBILITY_ALL:     return "ALL";
        case D3D12_SHADER_VISIBILITY_VERTEX:  return "VS";
        case D3D12_SHADER_VISIBILITY_PIXEL:   return "PS";
        case D3D12_SHADER_VISIBILITY_GEOMETRY: return "GS";
        case D3D12_SHADER_VISIBILITY_HULL:    return "HS";
        case D3D12_SHADER_VISIBILITY_DOMAIN:  return "DS";
        default:                              return "?";
    }
}

const char* RangeTypeName(D3D12_DESCRIPTOR_RANGE_TYPE t) {
    switch (t) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:     return "CBV";
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:     return "SRV";
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:     return "UAV";
        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: return "SAMPLER";
        default:                                  return "?";
    }
}

} // namespace

// ============================================================================
// Root signature 构建（绑定模型契约见 D3D12Pipeline.h 文件头）
// ============================================================================

bool BuildRootSignature(D3D12Device& device, const PipelineLayoutDesc& desc,
                        bool isCompute, D3D12RootSignatureLayout& out) {
    ID3D12Device* native = device.GetNativeDevice();
    if (!native) return false;

    // ---- 每集参数收集（表 ranges + 预算参数）----
    // H1 修复（stage 拆组）：main 绑定按 stageFlags 精确值分组，每组一个
    // 表参数（可见性=ToVisibility(组 stage)，见文件头契约 §2/§3）——异
    // stage 并入同一 ALL 表会让 VS 可见 b1 与 VS-only root 常量 b1 同
    // stage 重叠（CreateRootSignature 拒绝），且 next-free 计数会把 push
    // 推离 shader 落位。组内按 CBV→SRV→UAV 段序、段内 binding 升序切
    // 连续 range。sampler 与 root 常量（b 空间）寄存器字母不同，恒无
    // 重叠，保持单表不拆。
    struct MainGroupBuild {
        ShaderStage stage = ShaderStage::Unknown;
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;   // CBV→SRV→UAV 段序
    };
    struct SetBuild {
        std::vector<MainGroupBuild> mainGroups;
        std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges;
        u32 mainCount = 0;
        u32 samplerCount = 0;
    };
    std::vector<SetBuild> setBuilds(desc.setLayoutCount);
    // stageFlags 精确值 → 组（首次出现序；返回引用供 range 追加）。
    auto mainGroupOf = [](std::vector<MainGroupBuild>& groups,
                          ShaderStage stage) -> MainGroupBuild& {
        for (MainGroupBuild& g : groups) {
            if (g.stage == stage) return g;
        }
        groups.push_back(MainGroupBuild{stage, {}});
        return groups.back();
    };

    for (u32 setIdx = 0; setIdx < desc.setLayoutCount; ++setIdx) {
        D3D12DescriptorSetLayout* layout =
            device.GetDescriptorSetLayout(desc.setLayouts[setIdx]);
        if (!layout) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Pipeline] set " << setIdx << " layout handle unresolved" << std::endl;
            return false;
        }
        SetBuild& build = setBuilds[setIdx];

        // ---- 按 D3D12DescriptorSetLayout 区段序（CBV→SRV→UAV）
        // 逐连续 binding 段产 range（绑定模型契约 §2/§4）；连续段合并
        // 要求 stageFlags 完全相同（H1，见上）——异 stage 相邻绑定切段，
        // 各自落入对应 stage 组的表参数。----
        for (int typeIdx = 0; typeIdx < 3; ++typeIdx) {
            const D3D12_DESCRIPTOR_RANGE_TYPE type = typeIdx == 0
                ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                : typeIdx == 1 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                               : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            const utl::vector<DescriptorSetLayoutBinding>& bindings = layout->GetBindings();

            // 收集该类的 (binding, count, offset, stage)
            struct Item { u32 binding; u32 count; u32 offset; ShaderStage stage; };
            std::vector<Item> items;
            for (const auto& b : bindings) {
                D3D12DescriptorSetLayout::BindingClass info;
                if (!layout->QueryBinding(b.binding, info) || !info.hasMain) continue;
                if (info.mainType != type) continue;
                items.push_back({b.binding, info.count, info.mainOffset, b.stageFlags});
            }
            if (items.empty()) continue;
            std::sort(items.begin(), items.end(),
                      [](const Item& a, const Item& b) { return a.binding < b.binding; });

            // 连续且同 stageFlags 的 binding 段合并为一个 range。
            u32 segFirst = 0;
            while (segFirst < items.size()) {
                u32 segLast = segFirst;
                u32 covered = items[segFirst].binding + items[segFirst].count;
                while (segLast + 1 < items.size()
                       && items[segLast + 1].binding == covered
                       && items[segLast + 1].stage == items[segFirst].stage) {
                    ++segLast;
                    covered += items[segLast].count;
                }
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = type;
                range.NumDescriptors = covered - items[segFirst].binding;
                range.BaseShaderRegister = items[segFirst].binding;
                range.RegisterSpace = setIdx;  // set N ↔ space N（契约 §1）
                range.OffsetInDescriptorsFromTableStart = items[segFirst].offset;
                mainGroupOf(build.mainGroups, items[segFirst].stage)
                    .ranges.push_back(range);
                segFirst = segLast + 1;
            }
        }
        build.mainCount = layout->TotalDescriptorCount();

        // ---- sampler 段（s# 连续段；Sampler + CombinedImageSampler）----
        {
            const utl::vector<DescriptorSetLayoutBinding>& bindings = layout->GetBindings();
            struct SItem { u32 binding; u32 count; u32 offset; };
            std::vector<SItem> items;
            for (const auto& b : bindings) {
                D3D12DescriptorSetLayout::BindingClass info;
                if (!layout->QueryBinding(b.binding, info) || !info.hasSampler) continue;
                items.push_back({b.binding, info.count, info.samplerOffset});
            }
            std::sort(items.begin(), items.end(),
                      [](const SItem& a, const SItem& b) { return a.binding < b.binding; });
            u32 segFirst = 0;
            while (segFirst < items.size()) {
                u32 segLast = segFirst;
                u32 covered = items[segFirst].binding + items[segFirst].count;
                while (segLast + 1 < items.size()
                       && items[segLast + 1].binding == covered) {
                    ++segLast;
                    covered += items[segLast].count;
                }
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = covered - items[segFirst].binding;
                range.BaseShaderRegister = items[segFirst].binding;
                range.RegisterSpace = setIdx;
                range.OffsetInDescriptorsFromTableStart = items[segFirst].offset;
                build.samplerRanges.push_back(range);
                segFirst = segLast + 1;
            }
        }
        build.samplerCount = layout->SamplerCount();
    }

    // ---- push constants → root constants（M1 首个非空 range）----
    u32 pushBytes = 0;
    ShaderStage pushStage = ShaderStage::Unknown;
    for (u32 i = 0; i < desc.pushConstantRangeCount; ++i) {
        if (desc.pushConstantRanges && desc.pushConstantRanges[i].size > 0) {
            pushBytes = desc.pushConstantRanges[i].size;
            pushStage = desc.pushConstantRanges[i].stageFlags;
            break;
        }
    }
    const u32 constantsCount32 = (pushBytes + 3) / 4;

    // H1 修复：root 常量寄存器 = pushStage 可见的 set0 CBV binding 数
    // （按 binding stageFlags 过滤计数）——与 DXC 对 spirv-cross HLSL
    // push constant 的落位规则（该编译单元显式 b 寄存器数的 next-free
    // bN；binding 号 0..n-1 稠密时即 n，spike §10 /
    // d3d12_reflection_contract.py 冻结，M1Forward/M2ShadowDepth/M4GBuffer
    // vert 均为 b1，证据=dxc -dumpbin）严格一致。原实现按合并 range 的
    // NumDescriptors 计数：M1 语料把 VS|PS ViewData 与 PS-only LightData
    // 合成 b0-b1 range → push 推到 b2，而 vert shader 落位 b1 → 常量与
    // DXIL cbuffer 错位、模型矩阵读到 b1 槽 LightData+越界零（全黑帧
    // 根因 H1）。stage 拆组后 PS-only CBV 表对 VS 不可见，VS 可见 b 区
    // 与本寄存器无重叠（跨 stage 重叠本就合法）。
    // 边界登记：pushStage 可见 CBV 若非 0..n-1 稠密（b 空洞），DXC 填
    // 最小空洞、本式计数取 n——M1-M6 语料均稠密；复合 pushStage（多位）
    // 时按"任一位命中"计数，DXC 侧按单 stage 编译单元落位可能不同——
    // 两者出现时以反射契约测试的 push_constant_register 为准修订本式。
    u32 pushConstantRegister = 0;
    if (pushBytes > 0 && desc.setLayoutCount > 0) {
        D3D12DescriptorSetLayout* setLayout0 =
            device.GetDescriptorSetLayout(desc.setLayouts[0]);
        if (setLayout0) {
            const u32 pushBits = static_cast<u32>(pushStage);
            for (const auto& b : setLayout0->GetBindings()) {
                D3D12DescriptorSetLayout::BindingClass info;
                if (!setLayout0->QueryBinding(b.binding, info) || !info.hasMain) continue;
                if (info.mainType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV) continue;
                if ((static_cast<u32>(b.stageFlags) & pushBits) != 0) {
                    ++pushConstantRegister;
                }
            }
        }
    }

    // ---- 预算门禁（64-DWORD；超限显式失败）----
    {
        std::vector<D3D12RootParameterDesc> budgetParams;
        for (const SetBuild& b : setBuilds) {
            // H1 stage 拆组：每个组一个表参数（1 DWORD/表）。
            for (u32 g = 0; g < b.mainGroups.size(); ++g) {
                budgetParams.push_back({D3D12RootParameterDesc::Kind::DescriptorTable, 0, b.mainCount});
            }
            if (b.samplerCount > 0) budgetParams.push_back({D3D12RootParameterDesc::Kind::DescriptorTable, 0, b.samplerCount});
        }
        if (constantsCount32 > 0) {
            budgetParams.push_back({D3D12RootParameterDesc::Kind::Constants32Bit, constantsCount32, 0});
        }
        D3D12RootSignatureBudget budget =
            EvaluateRootSignature(budgetParams.data(),
                                  static_cast<u32>(budgetParams.size()));
        if (!budget.ok) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Pipeline] root signature budget exceeded: "
                << budget.dwords << "/64 DWORD — " << budget.reason << std::endl;
            return false;
        }
    }

    // ---- 序列化 ----
    std::vector<D3D12_ROOT_PARAMETER> rootParams;
    out.sets.resize(desc.setLayoutCount);
    for (u32 setIdx = 0; setIdx < desc.setLayoutCount; ++setIdx) {
        SetBuild& build = setBuilds[setIdx];
        D3D12SetRootParams& params = out.sets[setIdx];
        params.descriptorCount = build.mainCount;
        params.samplerCount = build.samplerCount;
        if (!build.mainGroups.empty()) {
            // H1：组参数连续排列（BindDescriptorSets 按 [tableParam,
            // tableParam+tableParamCount) 区间逐个设同一区段句柄）。
            params.tableParam = static_cast<u32>(rootParams.size());
            params.tableParamCount = static_cast<u32>(build.mainGroups.size());
            for (const MainGroupBuild& group : build.mainGroups) {
                D3D12_ROOT_PARAMETER param{};
                param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                param.DescriptorTable.NumDescriptorRanges =
                    static_cast<UINT>(group.ranges.size());
                param.DescriptorTable.pDescriptorRanges = group.ranges.data();
                param.ShaderVisibility = ToVisibility(group.stage);
                rootParams.push_back(param);
            }
        }
        if (!build.samplerRanges.empty()) {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable.NumDescriptorRanges =
                static_cast<UINT>(build.samplerRanges.size());
            param.DescriptorTable.pDescriptorRanges = build.samplerRanges.data();
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.samplerTableParam = static_cast<u32>(rootParams.size());
            rootParams.push_back(param);
        }
    }
    if (constantsCount32 > 0) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = pushConstantRegister;  // next-free bN（见上注）
        param.Constants.RegisterSpace = 0;
        param.Constants.Num32BitValues = constantsCount32;
        param.ShaderVisibility = ToVisibility(pushStage);
        out.constantsParam = static_cast<u32>(rootParams.size());
        out.constantsCount32 = constantsCount32;
        rootParams.push_back(param);
    }

    // ---- 合法性自检（H1 验收式）：同 stage 可见的根参数之间寄存器不得
    // 重叠（跨 stage 重叠合法）。stage 拆组 + pushStage 过滤计数在构造上
    // 保证不重叠；此处显式复核，冲突时先于 CreateRootSignature 给出可
    // 定位诊断（debug layer 只回 HRESULT）。----
    {
        struct RegSpan { u32 stageBits; char letter; u32 space; u32 first; u32 count; };
        std::vector<RegSpan> spans;
        const u32 kAllStages = static_cast<u32>(ShaderStage::Vertex)
            | static_cast<u32>(ShaderStage::Pixel)
            | static_cast<u32>(ShaderStage::Geometry)
            | static_cast<u32>(ShaderStage::Hull)
            | static_cast<u32>(ShaderStage::Domain)
            | static_cast<u32>(ShaderStage::Compute);
        for (const D3D12_ROOT_PARAMETER& p : rootParams) {
            u32 vis = kAllStages;  // ALL / compute 签名
            if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX)
                vis = static_cast<u32>(ShaderStage::Vertex);
            else if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL)
                vis = static_cast<u32>(ShaderStage::Pixel);
            else if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_GEOMETRY)
                vis = static_cast<u32>(ShaderStage::Geometry);
            else if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_HULL)
                vis = static_cast<u32>(ShaderStage::Hull);
            else if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_DOMAIN)
                vis = static_cast<u32>(ShaderStage::Domain);
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                spans.push_back({vis, 'b', p.Constants.RegisterSpace,
                                 p.Constants.ShaderRegister, 1});
            } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                    const D3D12_DESCRIPTOR_RANGE& range =
                        p.DescriptorTable.pDescriptorRanges[r];
                    const char letter =
                        range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? 'b'
                        : range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? 't'
                        : range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? 'u' : 's';
                    spans.push_back({vis, letter, range.RegisterSpace,
                                     range.BaseShaderRegister, range.NumDescriptors});
                }
            }
        }
        for (u32 i = 0; i < spans.size(); ++i) {
            for (u32 j = i + 1; j < spans.size(); ++j) {
                const RegSpan& spanA = spans[i];
                const RegSpan& spanB = spans[j];
                if (spanA.letter != spanB.letter || spanA.space != spanB.space) continue;
                if ((spanA.stageBits & spanB.stageBits) == 0) continue;  // 跨 stage 合法
                if (spanA.first < spanB.first + spanB.count
                    && spanB.first < spanA.first + spanA.count) {
                    PRIMAL_LOG_STREAM(Error, "RHI")
                        << "[D3D12Pipeline] root parameter register overlap: "
                        << spanA.letter << spanA.first << "+x" << spanA.count
                        << " vs " << spanB.letter << spanB.first << "+x" << spanB.count
                        << ",space" << spanA.space
                        << " (same-stage visibility) — layout rejected" << std::endl;
                    return false;
                }
            }
        }
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rsDesc.pParameters = rootParams.data();
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    // 图形管线带输入装配（IASetVertexBuffers 路径）需要该 flag；
    // compute root signature 不允许带（D3D12 规定 compute RS 必须可绑定 compute
    // ——flag 只影响 graphics 侧允许性，compute 用 NONE 保守值）。
    rsDesc.Flags = isCompute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                             : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Pipeline] D3D12SerializeRootSignature failed: "
            << (error ? reinterpret_cast<const char*>(error->GetBufferPointer()) : "")
            << std::endl;
        return false;
    }
    hr = native->CreateRootSignature(0, signature->GetBufferPointer(),
                                     signature->GetBufferSize(),
                                     IID_PPV_ARGS(&out.rootSignature));
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Pipeline] CreateRootSignature failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }
    // ---- H1 仪表：根签名布局逐参数记录——Windows 运行日志凭本段直接
    // 判定 push 常量落位与各组可见性（全黑帧排障凭据）。----
    {
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12Pipeline] root signature built: " << rootParams.size()
            << " params" << (isCompute ? " (compute)" : " (graphics)") << std::endl;
        for (u32 i = 0; i < rootParams.size(); ++i) {
            const D3D12_ROOT_PARAMETER& p = rootParams[i];
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                PRIMAL_LOG_STREAM(Info, "RHI")
                    << "[D3D12Pipeline]   param[" << i << "] constants b"
                    << p.Constants.ShaderRegister << ",space"
                    << p.Constants.RegisterSpace << " x"
                    << p.Constants.Num32BitValues << " visibility="
                    << VisibilityName(p.ShaderVisibility) << std::endl;
            } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                    const D3D12_DESCRIPTOR_RANGE& range =
                        p.DescriptorTable.pDescriptorRanges[r];
                    PRIMAL_LOG_STREAM(Info, "RHI")
                        << "[D3D12Pipeline]   param[" << i << "] table "
                        << VisibilityName(p.ShaderVisibility) << ": "
                        << RangeTypeName(range.RangeType) << " reg="
                        << range.BaseShaderRegister << ",space"
                        << range.RegisterSpace << " x" << range.NumDescriptors
                        << " +" << range.OffsetInDescriptorsFromTableStart
                        << std::endl;
                }
            }
        }
    }
    out.isCompute = isCompute;
    return true;
}

// ============================================================================
// D3D12PipelineLayout
// ============================================================================

D3D12PipelineLayout::D3D12PipelineLayout(D3D12Device& device, const PipelineLayoutDesc& desc)
    : RHIPipelineLayout(device, desc) {}

bool D3D12PipelineLayout::Initialize() {
    PipelineLayoutDesc desc{};
    desc.setLayoutCount = static_cast<u32>(setLayouts_.size());
    desc.setLayouts = setLayouts_.empty() ? nullptr : setLayouts_.data();
    desc.pushConstantRangeCount = static_cast<u32>(pushConstantRanges_.size());
    desc.pushConstantRanges = pushConstantRanges_.empty() ? nullptr : pushConstantRanges_.data();
    return BuildRootSignature(static_cast<D3D12Device&>(device_), desc,
                              /*isCompute=*/false, layout_);
}

void D3D12PipelineLayout::destroyImpl() {
    layout_.rootSignature.Reset();
    layout_.sets.clear();
}

// ============================================================================
// D3D12Pipeline
// ============================================================================

D3D12Pipeline::D3D12Pipeline(D3D12Device& device) : device_(device) {}

D3D12Pipeline::D3D12Pipeline(D3D12Pipeline&& other) noexcept
    : device_(other.device_),
      handle_(other.handle_),
      isCompute_(other.isCompute_),
      layout_(other.layout_),
      ownsLayout_(other.ownsLayout_),
      emptyRootSignature_(std::move(other.emptyRootSignature_)),
      iaTopology_(other.iaTopology_),
      pipeline_(std::move(other.pipeline_)),
      graphicsDesc_(std::move(other.graphicsDesc_)),
      computeDesc_(std::move(other.computeDesc_)) {
    other.handle_ = handles::INVALID_PIPELINE;
    other.layout_ = nullptr;
    other.ownsLayout_ = false;
    other.pipeline_.Reset();
}

D3D12Pipeline& D3D12Pipeline::operator=(D3D12Pipeline&& other) noexcept {
    if (this != &other) {
        // [R6/Windows] device_ 是 D3D12Device& 引用成员（不可重绑；C2280：
        // 经引用赋值=调用 Device 的已删除拷贝赋值）。move 赋值语义=引用
        // 保持指向构造期绑定的设备，与 move 构造器 device_(other.device_)
        // 的引用复制一致——不赋值即正确。
        handle_ = other.handle_;
        isCompute_ = other.isCompute_;
        layout_ = other.layout_;
        ownsLayout_ = other.ownsLayout_;
        emptyRootSignature_ = std::move(other.emptyRootSignature_);
        iaTopology_ = other.iaTopology_;
        pipeline_ = std::move(other.pipeline_);
        graphicsDesc_ = std::move(other.graphicsDesc_);
        computeDesc_ = std::move(other.computeDesc_);
        other.handle_ = handles::INVALID_PIPELINE;
        other.layout_ = nullptr;
        other.ownsLayout_ = false;
        other.pipeline_.Reset();
    }
    return *this;
}

D3D12Pipeline::~D3D12Pipeline() { Destroy(); }

bool D3D12Pipeline::InitializeGraphics(const GraphicsPipelineDesc& desc) {
    ID3D12Device* native = device_.GetNativeDevice();
    if (!native) return false;
    isCompute_ = false;
    graphicsDesc_ = desc;

    // root signature：优先管线布局句柄；无效则自建空布局（对齐 Vulkan
    // ownedLayout_ fallback 语义）。
    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        layout_ = device_.GetPipelineLayout(desc.layout);
        if (!layout_) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Pipeline] layout handle unresolved" << std::endl;
            return false;
        }
    } else {
        PipelineLayoutDesc empty{};
        D3D12RootSignatureLayout emptyLayout;
        if (!BuildRootSignature(device_, empty, false, emptyLayout)) return false;
        emptyRootSignature_ = emptyLayout.rootSignature;
        ownsLayout_ = true;
    }

    D3D12Shader* vs = device_.GetShader(desc.vertexShader);
    if (!vs) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Pipeline] vertex shader handle unresolved (DXIL must be "
            << "created via CreateShader before pipeline)" << std::endl;
        return false;
    }
    // depth-only 契约（R12/Windows）：pixelShader == handles::INVALID_SHADER
    // 合法——PSO PixelShader 留空字节码（pso{} 零初始化即 {}），D3D12 规范
    // 下完全合法，是 depth-only 渲染的标准形态（OMSetRenderTargets 0 RT +
    // DSV）。先例与参照：
    //   - legacy 分支 GPass 深度管线：Engine/Graphics/Direct3D12/D3D12GPass.cpp
    //     的 depth_pipeline_states[]——depth pass 用独立无 PS PSO 绘制；
    //   - 宿主 M2 阴影段：TestRHISponzaShared.cpp "dpd.pixelShader =
    //     INVALID_SHADER // depth-only"（M2ShadowDepth.vert 无 PS）；
    //   - 用户参照的 HLSL 惯例样例 EngineTest/TestShader.hlsl（Common.hlsli
    //     include + ELEMENTS_TYPE 编译期配置，VS-only 变体同源）；
    //   - Vulkan 侧 VulkanPipeline::addStage 对 INVALID_SHADER 跳过——RHI
    //     层语义本就允许无 PS 图形管线，本处对齐（此前成对要求是后端偏差，
    //     R12 修复）。
    // 注意 GetShader(INVALID_SHADER) 静默返回 nullptr，与"句柄失效"不可区分，
    // 故必须先判 INVALID 再解析；句柄非 INVALID 但解析失败仍是真错误（不掩盖）。
    D3D12Shader* ps = nullptr;
    if (desc.pixelShader != handles::INVALID_SHADER) {
        ps = device_.GetShader(desc.pixelShader);
        if (!ps) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Pipeline] pixel shader handle unresolved (non-null handle "
                << "must resolve; pass INVALID_SHADER for depth-only)" << std::endl;
            return false;
        }
    }

    // ---- 输入布局（顶点绑定/属性直译；32B stride 等由 desc 原样承载）----
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    for (const auto& attr : desc.vertexAttributes) {
        D3D12_INPUT_ELEMENT_DESC element{};
        // spirv-cross HLSL 语义名约定：location N → TEXCOORDN
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = attr.location;
        element.Format = static_cast<DXGI_FORMAT>(ToDxgiFormat(attr.format));
        element.InputSlot = attr.binding;
        element.AlignedByteOffset = attr.offset;
        // [R6/Windows] C3535：const auto* 无法从 nullptr 推导（MSVC 推不出
        // 依据）。显式类型=desc.vertexBindings（utl::vector<VertexInputBinding>，
        // RHIDevice.h L90）的元素指针。
        const VertexInputBinding* binding = nullptr;
        for (const auto& b : desc.vertexBindings) {
            if (b.binding == attr.binding) { binding = &b; break; }
        }
        element.InputSlotClass = (binding && !binding->perVertex)
                                     ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate =
            (binding && !binding->perVertex) ? 1 : 0;
        inputElements.push_back(element);
    }

    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ToTopology(desc.topology, topologyType, iaTopology_);

    // ---- 光栅化（cull 直译契约见 ToCullMode，H2 修复）----
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = desc.fillMode == FillMode::Wireframe
                              ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = ToCullMode(desc.cullMode);
    rasterizer.FrontCounterClockwise = FALSE;  // CW=正面：与 Vulkan frontFace=CW 同义（负 viewport 已补偿 NDC Y 语义差）
    // F2-4（M2）bias 直译：VulkanPipeline 同样消费三元组（静态
    // depthBiasConstantFactor/SlopeFactor/Clamp）。语义差异记录在案：
    //   1) D3D12 DepthBias 是整数光栅化单位，Vulkan constantFactor 是
    //      乘以实现相关最小可分辨深度的 float——同一数值两端单位不保证
    //      相同（经典跨 API bias 单位差）；M2 共同路径三元组 = 0/0/0
    //      （生产阴影管线同值），等价性平凡成立，bias 全走 shader 侧。
    //   2) DepthBiasClamp 两端语义同（|bias| 上限）；注意 VulkanPipeline
    //      侧额外把 clamp!=0 映射到 depthClampEnable=TRUE（近/远裁剪钳制）
    //      ——那是既有口径，M2 路径 clamp=0 不触发。
    rasterizer.DepthBias = static_cast<INT>(desc.depthBias);
    rasterizer.DepthBiasClamp = desc.depthBiasClamp;
    rasterizer.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // ---- 深度模板（§7.2-2：[0,1] 正向 Z，Less 直译，清 1.0）----
    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = desc.enableDepthTest ? TRUE : FALSE;
    depthStencil.DepthWriteMask = desc.enableDepthWrite
                                      ? D3D12_DEPTH_WRITE_MASK_ALL
                                      : D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencil.DepthFunc = ToComparisonFunc(desc.depthFunc);
    depthStencil.StencilEnable = desc.enableStencilTest ? TRUE : FALSE;
    depthStencil.StencilReadMask = desc.stencilReadMask;
    depthStencil.StencilWriteMask = desc.stencilWriteMask;
    depthStencil.FrontFace.StencilFailOp = ToStencilOp(desc.frontStencil.failOp);
    depthStencil.FrontFace.StencilDepthFailOp = ToStencilOp(desc.frontStencil.depthFailOp);
    depthStencil.FrontFace.StencilPassOp = ToStencilOp(desc.frontStencil.passOp);
    depthStencil.FrontFace.StencilFunc = ToComparisonFunc(desc.frontStencil.func);
    depthStencil.BackFace.StencilFailOp = ToStencilOp(desc.backStencil.failOp);
    depthStencil.BackFace.StencilDepthFailOp = ToStencilOp(desc.backStencil.depthFailOp);
    depthStencil.BackFace.StencilPassOp = ToStencilOp(desc.backStencil.passOp);
    depthStencil.BackFace.StencilFunc = ToComparisonFunc(desc.backStencil.func);

    // ---- 混合（单 RT 直译；blendConstants 属帧级状态——SetBlendConstants M2+）----
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    for (u32 i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        blend.RenderTarget[i].BlendEnable = desc.enableBlend ? TRUE : FALSE;
        blend.RenderTarget[i].SrcBlend = ToBlend(desc.srcColorBlendFactor);
        blend.RenderTarget[i].DestBlend = ToBlend(desc.dstColorBlendFactor);
        blend.RenderTarget[i].BlendOp = ToBlendOp(desc.colorBlendOp);
        blend.RenderTarget[i].SrcBlendAlpha = ToBlend(desc.srcAlphaBlendFactor);
        blend.RenderTarget[i].DestBlendAlpha = ToBlend(desc.dstAlphaBlendFactor);
        blend.RenderTarget[i].BlendOpAlpha = ToBlendOp(desc.alphaBlendOp);
        blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[i].LogicOpEnable = FALSE;
        blend.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    // ---- PSO ----
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = GetRootSignature();
    pso.VS = vs->ByteCode();
    // depth-only：ps == nullptr → PSO PixelShader 保持零初始化空字节码
    // （契约见上方 shader 解析段注释）；0 RT + DSV 的绑定路径由
    // D3D12CommandBuffer::BeginRenderPass 承载（rtCount=0 + DSV，
    // 无 PS 专用断言）。
    if (ps) pso.PS = ps->ByteCode();
    // GS/HS/DS：DXIL 字节码直通（M1 语料未用；提供即接线）
    if (desc.geometryShader != handles::INVALID_SHADER) {
        if (D3D12Shader* gs = device_.GetShader(desc.geometryShader)) pso.GS = gs->ByteCode();
    }
    if (desc.hullShader != handles::INVALID_SHADER) {
        if (D3D12Shader* hs = device_.GetShader(desc.hullShader)) pso.HS = hs->ByteCode();
    }
    if (desc.domainShader != handles::INVALID_SHADER) {
        if (D3D12Shader* ds = device_.GetShader(desc.domainShader)) pso.DS = ds->ByteCode();
    }
    pso.StreamOutput = {};
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = rasterizer;
    pso.DepthStencilState = depthStencil;
    pso.InputLayout.pInputElementDescs =
        inputElements.empty() ? nullptr : inputElements.data();
    pso.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    pso.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pso.PrimitiveTopologyType = topologyType;
    pso.NumRenderTargets = desc.renderTargetCount;
    for (u32 i = 0; i < desc.renderTargetCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        pso.RTVFormats[i] = static_cast<DXGI_FORMAT>(ToDxgiFormat(desc.renderTargetFormats[i]));
    }
    pso.DSVFormat = desc.depthStencilFormat != DataFormat::Unknown
                        ? static_cast<DXGI_FORMAT>(ToDxgiFormat(desc.depthStencilFormat))
                        : DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;
    pso.SampleDesc.Quality = 0;
    pso.NodeMask = 0;
    // [R6/Windows] CachedPSO 是 D3D12_CACHED_PIPELINE_STATE 结构体值（非
    // 指针；官方头 L2600/2609）——nullptr 赋值 C2679。置零空缓存=语义相同。
    pso.CachedPSO = {};
    pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HRESULT hr = native->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_));
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Pipeline] CreateGraphicsPipelineState failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }
    return true;
}

bool D3D12Pipeline::InitializeCompute(const ComputePipelineDesc& desc) {
    ID3D12Device* native = device_.GetNativeDevice();
    if (!native) return false;
    isCompute_ = true;
    computeDesc_ = desc;

    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        layout_ = device_.GetPipelineLayout(desc.layout);
        if (!layout_) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Pipeline] compute layout handle unresolved" << std::endl;
            return false;
        }
    } else {
        PipelineLayoutDesc empty{};
        D3D12RootSignatureLayout emptyLayout;
        if (!BuildRootSignature(device_, empty, true, emptyLayout)) return false;
        emptyRootSignature_ = emptyLayout.rootSignature;
        ownsLayout_ = true;
    }

    D3D12Shader* cs = device_.GetShader(desc.computeShader);
    if (!cs || cs->GetStage() != ShaderStage::Compute) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Pipeline] compute shader handle unresolved" << std::endl;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = GetRootSignature();
    pso.CS = cs->ByteCode();
    pso.NodeMask = 0;
    // [R6/Windows] CachedPSO 是 D3D12_CACHED_PIPELINE_STATE 结构体值（非
    // 指针；官方头 L2600/2609）——nullptr 赋值 C2679。置零空缓存=语义相同。
    pso.CachedPSO = {};
    pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    HRESULT hr = native->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Pipeline] CreateComputePipelineState failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }
    return true;
}

ID3D12RootSignature* D3D12Pipeline::GetRootSignature() const {
    if (layout_) return layout_->GetRootSignature();
    return ownsLayout_ ? emptyRootSignature_.Get() : nullptr;
}

void D3D12Pipeline::Destroy() {
    pipeline_.Reset();
    emptyRootSignature_.Reset();
    layout_ = nullptr;
    ownsLayout_ = false;
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
