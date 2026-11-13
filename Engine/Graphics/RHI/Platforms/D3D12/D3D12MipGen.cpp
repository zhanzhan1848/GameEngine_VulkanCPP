// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12MipGen.cpp — GenerateMipmaps 设备级渲染资产实现（R13/T2）。
#include "D3D12MipGen.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <utility>

#include "D3D12MipGenShaders.h"  // checked-in 静态嵌入头（scripts/build_d3d12_mipgen.py 离线预编译生成，R14-2）

#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi::d3d12 {

bool D3D12MipGenerator::Initialize(ID3D12Device* device) {
    if (!device) return false;
    // 根签名：[0] SRV 描述符表（t0，pixel 可见）+ 静态线性采样器 s0
    //（静态采样器不占 sampler 堆槽位——mip gen 无须动 sampler 堆/双写）。
    // 序列化对齐 D3D12Pipeline::BuildRootSignature 先例（v1 无版本 API +
    // D3D12SerializeRootSignature——同一链接面已在 Windows 构建验证）。
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;      // t0
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER tableParam{};
    tableParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tableParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    tableParam.DescriptorTable.NumDescriptorRanges = 1;
    tableParam.DescriptorTable.pDescriptorRanges = &range;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;  // 双线性=半尺寸下 2x2 box
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &tableParam;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;  // 无输入装配（SV_VertexID）

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12MipGen] D3D12SerializeRootSignature failed: "
            << (error ? reinterpret_cast<const char*>(error->GetBufferPointer()) : "")
            << std::endl;
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12MipGen] CreateRootSignature failed" << std::endl;
        return false;
    }
    device_ = device;
    return true;
}

void D3D12MipGenerator::Shutdown() {
    pipelines_.clear();
    rootSignature_.Reset();
    device_ = nullptr;
}

bool D3D12MipGenerator::HasShaderBytes() const {
    return kMipGenVsSize > 0 && kMipGenPsSize > 0;
}

ID3D12PipelineState* D3D12MipGenerator::GetOrCreatePipeline(UINT rtvFormat) {
    if (!device_ || !rootSignature_ || !HasShaderBytes()) return nullptr;
    for (auto& [fmt, pso] : pipelines_) {
        if (fmt == rtvFormat) return pso.Get();
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS.pShaderBytecode = kMipGenVsBytes;
    psoDesc.VS.BytecodeLength = kMipGenVsSize;
    psoDesc.PS.pShaderBytecode = kMipGenPsBytes;
    psoDesc.PS.BytecodeLength = kMipGenPsSize;
    // 无顶点输入（SV_VertexID 全屏三角形）、无深度、无混合、不剔除。
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.InputLayout = {};
    psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = static_cast<DXGI_FORMAT>(rtvFormat);
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.NodeMask = 0;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12MipGen] PSO creation failed (RTV format 0x" << std::hex
            << rtvFormat << std::dec << ")" << std::endl;
        return nullptr;
    }
    pipelines_.push_back({rtvFormat, std::move(pso)});
    return pipelines_.back().second.Get();
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
