/**
 * @file VulkanShader.h
 * @brief Vulkan RHI shader 实现
 * @details Phase 4: SPIR-V 字节码 → VkShaderModule。
 *          独立 class(非 RHIResource),由 VulkanDevice::shaderAllocator_ 持有。
 *          每个 shader 持自己的 stage + entryPoint 字符串,VulkanPipeline 创建时取用。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanShader {
    friend class VulkanDevice;
    friend class VulkanPipeline;
public:
    VulkanShader(VulkanDevice& device, const void* data, size_t size,
                 ShaderStage stage, const char* entryPoint);
    VulkanShader(VulkanShader&& other) noexcept;
    VulkanShader& operator=(VulkanShader&& other) noexcept;
    VulkanShader(const VulkanShader&) = delete;
    VulkanShader& operator=(const VulkanShader&) = delete;
    ~VulkanShader();

    bool Initialize();
    void Destroy();

    /// 重新加载 SPIR-V(热重载用)— Destroy + Initialize
    bool Reload(const void* data, size_t size);

    VkShaderModule    GetNativeModule() const { return module_; }
    ShaderStage       GetStage() const         { return stage_; }
    const char*       GetEntryPoint() const    { return entryPoint_.c_str(); }

    ShaderHandle      GetHandle() const        { return handle_; }
    void              SetHandle(ShaderHandle h){ handle_ = h; }

private:
    VulkanDevice& device_;
    ShaderHandle  handle_{handles::INVALID_SHADER};
    ShaderStage   stage_{ShaderStage::Unknown};

    std::vector<u8> spirv_;          // SPIR-V bytes(own copy,支持 Reload)
    std::string     entryPoint_;     // 例如 "main"
    VkShaderModule  module_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
