/**
 * @file MetalShader.h
 * @brief Metal着色器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class MetalDevice;

class MetalShader {
public:
    MetalShader(MetalDevice& device, const void* data, size_t size, ShaderStage stage, const char* entryPoint);
    ~MetalShader();

    bool Initialize();
    
    /**
     * @brief 热重载 Shader
     */
    bool Reload(const void* data, size_t size);
    
    void Destroy();

    MTL::Function* GetFunction() const { return function_; }
    ShaderStage GetStage() const { return stage_; }

    void SetHandle(ShaderHandle handle) { handle_ = handle; }
    ShaderHandle GetHandle() const { return handle_; }

private:
    MetalDevice& device_;
    ShaderHandle handle_ = handles::INVALID_SHADER;
    const void* data_;
    size_t size_;
    ShaderStage stage_;
    std::string entryPoint_;

    MTL::Library* library_ = nullptr;
    MTL::Function* function_ = nullptr;
};

} // namespace primal::graphics::rhi
