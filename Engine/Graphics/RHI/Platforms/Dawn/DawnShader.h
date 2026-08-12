#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"
#include <string>

namespace primal::graphics::rhi {

class DawnDevice;

class DawnShader {
    friend class DawnDevice;
public:
    ~DawnShader();
    WGPUShaderModule GetModule() const { return wgpuModule_; }
    ShaderStage GetStage() const { return stage_; }
    const char* GetEntryPoint() const { return entryPoint_.c_str(); }

public:
    DawnShader(DawnDevice& device);

private:
    bool Initialize(const void* data, size_t size, ShaderStage stage, const char* entryPoint);
    void Destroy();

    DawnDevice& device_;
    WGPUShaderModule wgpuModule_ = nullptr;
    ShaderStage stage_ = ShaderStage::Unknown;
    std::string entryPoint_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
