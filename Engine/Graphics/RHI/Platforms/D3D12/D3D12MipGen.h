// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12MipGen.h — GenerateMipmaps 的设备级渲染资产（R13/T2）。
//
// 形态：一个根签名（SRV t0 描述符表 + 静态线性采样器 s0——不占 sampler
// 堆槽位）+ 按 RTV 格式惰性创建的图形 PSO 缓存（PSO 的 RTVFormats[0]
// 必须与目标纹理格式一致，格式数有限，逐格式缓存）。shader 字节来自
// checked-in 静态嵌入头 D3D12MipGenShaders.h（MipGen.hlsl 离线 dxc
// 预编译 → cso → 字节数组，scripts/build_d3d12_mipgen.py 重生成——
// R14-2 起构建期零 HLSL 编译器依赖，Windows 0x80070459 根治）；
// HasShaderBytes() 仅防御性检查（静态头正常恒有字节）。
//
// 线程模型：设备单线程使用（与 D3D12Device 其余子系统一致），无锁。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi::d3d12 {

class D3D12MipGenerator {
public:
    D3D12MipGenerator() = default;
    ~D3D12MipGenerator() = default;
    D3D12MipGenerator(const D3D12MipGenerator&) = delete;
    D3D12MipGenerator& operator=(const D3D12MipGenerator&) = delete;

    /// 创建根签名（格式无关，设备初始化期一次）。失败=false。
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    /// 嵌入 shader 字节可用性（checked-in 静态头恒有字节；防御性检查）。
    bool HasShaderBytes() const;

    /// 该 RTV 格式的 PSO（惰性创建 + 缓存）。失败（含字节缺失）=nullptr。
    ID3D12PipelineState* GetOrCreatePipeline(UINT rtvFormat);

    ID3D12RootSignature* GetRootSignature() const { return rootSignature_.Get(); }

private:
    ID3D12Device* device_ = nullptr;  // 非持有
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    /// (RTV 格式 → PSO) 小缓存：格式种类个位数，线性查找即够。
    utl::vector<std::pair<UINT, Microsoft::WRL::ComPtr<ID3D12PipelineState>>> pipelines_;
};

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
