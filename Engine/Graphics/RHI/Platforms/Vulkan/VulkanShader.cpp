/**
 * @file VulkanShader.cpp
 * @brief VulkanShader 实现
 * @details SPIR-V bytes → VkShaderModule。Vulkan 不支持运行时 GLSL 编译,
 *          caller 必须提供预编译的 SPIR-V(离线 glslangValidator/DXC)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanShader.h"
#include "VulkanDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <cstring>

namespace primal::graphics::rhi {

VulkanShader::VulkanShader(VulkanDevice& device, const void* data, size_t size,
                           ShaderStage stage, const char* entryPoint)
    : device_(device),
      stage_(stage),
      entryPoint_(entryPoint ? entryPoint : "main") {
    if (data && size > 0) {
        spirv_.resize(size);
        std::memcpy(spirv_.data(), data, size);
    }
}

VulkanShader::VulkanShader(VulkanShader&& other) noexcept
    : device_(other.device_),
      handle_(other.handle_),
      stage_(other.stage_),
      spirv_(std::move(other.spirv_)),
      entryPoint_(std::move(other.entryPoint_)),
      module_(other.module_) {
    other.module_ = VK_NULL_HANDLE;
    other.handle_ = handles::INVALID_SHADER;
}

VulkanShader& VulkanShader::operator=(VulkanShader&& other) noexcept {
    if (this != &other) {
        Destroy();
        // 不能改 device_ 引用 — 假设同 device
        handle_ = other.handle_;
        stage_ = other.stage_;
        spirv_ = std::move(other.spirv_);
        entryPoint_ = std::move(other.entryPoint_);
        module_ = other.module_;
        other.module_ = VK_NULL_HANDLE;
        other.handle_ = handles::INVALID_SHADER;
    }
    return *this;
}

VulkanShader::~VulkanShader() {
    Destroy();
}

bool VulkanShader::Initialize() {
    if (module_ != VK_NULL_HANDLE) return true;  // 已初始化
    if (spirv_.empty()) {
        std::cerr << "[VulkanShader] No SPIR-V data" << std::endl;
        return false;
    }
    // SPIR-V 必须按 4 字节对齐(size 必须是 4 的倍数)
    if (spirv_.size() % 4 != 0) {
        std::cerr << "[VulkanShader] SPIR-V size " << spirv_.size()
                  << " not multiple of 4" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv_.size();
    ci.pCode = reinterpret_cast<const u32*>(spirv_.data());

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    if (vkCreateShaderModule(dev, &ci, nullptr, &module_) != VK_SUCCESS) {
        std::cerr << "[VulkanShader] vkCreateShaderModule failed" << std::endl;
        return false;
    }

    // Debug label
    const char* name = nullptr;  // Phase 4 暂无 name 字段;后续若加再传
    (void)name;
    return true;
}

void VulkanShader::Destroy() {
    if (module_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    device_.GetGarbageCollector().DeferredDestroy([dev, m = module_]() {
        if (dev != VK_NULL_HANDLE && m != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m, nullptr);
    });
    module_ = VK_NULL_HANDLE;
}

bool VulkanShader::Reload(const void* data, size_t size) {
    Destroy();
    spirv_.clear();
    if (data && size > 0) {
        spirv_.resize(size);
        std::memcpy(spirv_.data(), data, size);
    }
    return Initialize();
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
