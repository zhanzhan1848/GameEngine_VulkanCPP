// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Shader.h — ShaderHandle 承载体（M1 F1-7：DXIL 字节码）。
//
// 字节码语义（对齐 Vulkan CreateShader 的字节省约定——Vulkan 侧直传
// SPIR-V 字节，加载路径归测试宿主/RenderSystem）：
//   - CreateShader(data, size, stage, entryPoint) 的 data = DXIL 容器字节
//     （原生 HLSL 经 DXC 离线编译，每个入口一个 .cso）。
//     宿主从 EngineTest/shaders/d3d12/ 读取清单指定的相对路径。
//   - 入口由离线编译的 -E 选定，PSO 直接消费这份字节码。
//   - Initialize 校验 DXBC 容器魔数（"DXBC"）——错把 SPIR-V 字节喂进来时
//     显式失败，不静默。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

class D3D12Shader {
    friend class primal::graphics::rhi::D3D12Device;
    friend class D3D12Pipeline;
public:
    D3D12Shader(D3D12Device& device, const void* data, size_t size,
                ShaderStage stage, const char* entryPoint);
    D3D12Shader(D3D12Shader&& other) noexcept;
    D3D12Shader& operator=(D3D12Shader&& other) noexcept;
    D3D12Shader(const D3D12Shader&) = delete;
    D3D12Shader& operator=(const D3D12Shader&) = delete;
    ~D3D12Shader();

    bool Initialize();
    void Destroy();
    bool Reload(const void* data, size_t size);

    /// PSO 用的字节码视图（指向内部副本，生命周期随本对象）。
    D3D12_SHADER_BYTECODE ByteCode() const;

    ShaderStage GetStage() const { return stage_; }
    const char* GetEntryPoint() const { return "main"; }  // 恒 main（见文件头）

    ShaderHandle GetHandle() const { return handle_; }
    void SetHandle(ShaderHandle h) { handle_ = h; }

private:
    D3D12Device& device_;
    ShaderHandle handle_{handles::INVALID_SHADER};
    ShaderStage stage_{ShaderStage::Unknown};
    std::vector<u8> dxil_;       // DXIL 容器字节（自有副本，支持 Reload）
    bool initialized_ = false;
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
