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

/// CullMode 翻转契约（§7.2-1）：负 viewport 高度使屏幕空间绕序相对 Vulkan
/// 翻转，PSO cull 取反义（RHI Front→D3D12_BACK，Back→D3D12_FRONT，None 不变）。
D3D12_CULL_MODE ToCullModeFlipped(CullMode mode) {
    switch (mode) {
        case CullMode::Front: return D3D12_CULL_MODE_BACK;
        case CullMode::Back:  return D3D12_CULL_MODE_FRONT;
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

} // namespace

// ============================================================================
// Root signature 构建（绑定模型契约见 D3D12Pipeline.h 文件头）
// ============================================================================

bool BuildRootSignature(D3D12Device& device, const PipelineLayoutDesc& desc,
                        bool isCompute, D3D12RootSignatureLayout& out) {
    ID3D12Device* native = device.GetNativeDevice();
    if (!native) return false;

    // ---- 每集参数收集（表 ranges + 预算参数）----
    struct SetBuild {
        std::vector<D3D12_DESCRIPTOR_RANGE> mainRanges;    // CBV→SRV→UAV 段序
        std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges;
        u32 mainCount = 0;
        u32 samplerCount = 0;
    };
    std::vector<SetBuild> setBuilds(desc.setLayoutCount);

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
        // 逐连续 binding 段产 range（绑定模型契约 §2/§4）----
        for (int typeIdx = 0; typeIdx < 3; ++typeIdx) {
            const D3D12_DESCRIPTOR_RANGE_TYPE type = typeIdx == 0
                ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                : typeIdx == 1 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                               : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            const utl::vector<DescriptorSetLayoutBinding>& bindings = layout->GetBindings();

            // 收集该类的 (binding, count, offset)
            struct Item { u32 binding; u32 count; u32 offset; };
            std::vector<Item> items;
            for (const auto& b : bindings) {
                D3D12DescriptorSetLayout::BindingClass info;
                if (!layout->QueryBinding(b.binding, info) || !info.hasMain) continue;
                if (info.mainType != type) continue;
                items.push_back({b.binding, info.count, info.mainOffset});
            }
            if (items.empty()) continue;
            std::sort(items.begin(), items.end(),
                      [](const Item& a, const Item& b) { return a.binding < b.binding; });

            // 连续 binding 段合并为一个 range。
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
                range.RangeType = type;
                range.NumDescriptors = covered - items[segFirst].binding;
                range.BaseShaderRegister = items[segFirst].binding;
                range.RegisterSpace = setIdx;  // set N ↔ space N（契约 §1）
                range.OffsetInDescriptorsFromTableStart = items[segFirst].offset;
                build.mainRanges.push_back(range);
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

    // M7 F7-7 静态终审修复：root 常量寄存器 = space0 的"下一个空闲 bN"，
    // 与 DXIL push constant 固化落位规则严格一致（spike §10 /
    // d3d12_reflection_contract.py 固化：spirv-cross 把 push_constant 分配
    // 到 next-free bN——M1Forward/M2ShadowDepth/M4GBuffer 均为 b1，证据=
    ///tmp/d3d12_m6_evidence/reflection_contract.log "push_constant_register":"b1"）。
    // 原实现硬编码 ShaderRegister=0：root 常量落在 b0 与 set0 的 b0 CBV
    // range 重叠，且 shader 声明在 b1 的 cbuffer 永远收不到 PushConstants
    // 数据（M1-M6 全部带 push constant 的 D3D12 绘制会渲染错）。边界登记：
    // 绑定布局若存在 b 寄存器空洞（如 b0,b2），spirv-cross 填洞（b1）、
    // 本规则取 max+1（b3）——M1-M6 语料 b 区间均连续无空洞；出现空洞布局
    // 时以反射契约测试的 push_constant_register 为准修订本式。
    u32 pushConstantRegister = 0;
    if (!setBuilds.empty()) {
        for (const D3D12_DESCRIPTOR_RANGE& r : setBuilds[0].mainRanges) {
            if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && r.RegisterSpace == 0) {
                pushConstantRegister = std::max(
                    pushConstantRegister, r.BaseShaderRegister + r.NumDescriptors);
            }
        }
    }

    // ---- 预算门禁（64-DWORD；超限显式失败）----
    {
        std::vector<D3D12RootParameterDesc> budgetParams;
        for (const SetBuild& b : setBuilds) {
            if (b.mainCount > 0) budgetParams.push_back({D3D12RootParameterDesc::Kind::DescriptorTable, 0, b.mainCount});
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
        if (!build.mainRanges.empty()) {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable.NumDescriptorRanges =
                static_cast<UINT>(build.mainRanges.size());
            param.DescriptorTable.pDescriptorRanges = build.mainRanges.data();
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.tableParam = static_cast<u32>(rootParams.size());
            rootParams.push_back(param);
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
        device_ = other.device_;
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
    D3D12Shader* ps = device_.GetShader(desc.pixelShader);
    if (!vs || !ps) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Pipeline] vertex/pixel shader handle unresolved (DXIL must be "
            << "created via CreateShader before pipeline)" << std::endl;
        return false;
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
        const auto* binding = nullptr;
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

    // ---- 光栅化（cull 翻转契约见 ToCullModeFlipped）----
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = desc.fillMode == FillMode::Wireframe
                              ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = ToCullModeFlipped(desc.cullMode);
    rasterizer.FrontCounterClockwise = FALSE;  // 负 viewport + cull 翻转已补偿绕序
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
    pso.PS = ps->ByteCode();
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
    pso.CachedPSO = nullptr;
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
    pso.CachedPSO = nullptr;
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
