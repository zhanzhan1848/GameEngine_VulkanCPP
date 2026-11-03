// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Shader.cpp — DXIL 字节码承载实现（M1 F1-7）。
#include "D3D12Shader.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <cstring>

#include "Engine/Common/CoreDiag.h"

#ifndef PRIMAL_LOG_STREAM
#define PRIMAL_LOG_STREAM(level, channel) primal::diag::StreamProxy(level, channel)
#endif

namespace primal::graphics::rhi::d3d12 {

D3D12Shader::D3D12Shader(D3D12Device& device, const void* data, size_t size,
                         ShaderStage stage, const char* /*entryPoint*/)
    : device_(device), stage_(stage) {
    // entryPoint 恒 main（文件头契约——主链把 DXIL 入口统一重命名为 main）。
    if (data && size > 0) {
        dxil_.resize(size);
        std::memcpy(dxil_.data(), data, size);
    }
}

D3D12Shader::D3D12Shader(D3D12Shader&& other) noexcept
    : device_(other.device_),
      handle_(other.handle_),
      stage_(other.stage_),
      dxil_(std::move(other.dxil_)),
      initialized_(other.initialized_) {
    other.handle_ = handles::INVALID_SHADER;
    other.initialized_ = false;
}

D3D12Shader& D3D12Shader::operator=(D3D12Shader&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        handle_ = other.handle_;
        stage_ = other.stage_;
        dxil_ = std::move(other.dxil_);
        initialized_ = other.initialized_;
        other.handle_ = handles::INVALID_SHADER;
        other.initialized_ = false;
    }
    return *this;
}

D3D12Shader::~D3D12Shader() { Destroy(); }

bool D3D12Shader::Initialize() {
    if (dxil_.size() < 4) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Shader] empty shader bytecode" << std::endl;
        return false;
    }
    // DXBC 容器魔数校验（DXIL 以 DXBC 容器承载；dxc -Fo 恒产该容器）。
    if (std::memcmp(dxil_.data(), "DXBC", 4) != 0) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Shader] bytecode is not a DXBC/DXIL container (magic mismatch) — "
            << "expected dxc output, size=" << dxil_.size() << std::endl;
        return false;
    }
    initialized_ = true;
    return true;
}

void D3D12Shader::Destroy() {
    dxil_.clear();
    dxil_.shrink_to_fit();
    initialized_ = false;
}

bool D3D12Shader::Reload(const void* data, size_t size) {
    if (!data || size < 4 || std::memcmp(data, "DXBC", 4) != 0) return false;
    dxil_.resize(size);
    std::memcpy(dxil_.data(), data, size);
    initialized_ = true;
    return true;
}

D3D12_SHADER_BYTECODE D3D12Shader::ByteCode() const {
    D3D12_SHADER_BYTECODE bc{};
    bc.pShaderBytecode = dxil_.data();
    bc.BytecodeLength = dxil_.size();
    return bc;
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
