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
#include "Engine/Common/CoreLogStream.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <cstring>
#include <string>

namespace primal::graphics::rhi {

namespace detail {

// R7-WIN 诊断:非 SPIR-V 字节判类 + 前 32 字节 hex 预览。纯日志辅助,
// 不改任何加载行为。分类优先级:DXBC 魔数(Windows .cso 镜像被
// platform 分支误喂)→ 全零 → 可打印文本(GLSL/WGSL/Metal 源文件被
// 当作字节码加载)→ 其余归"未识别二进制"。
std::string DescribeBadSpirvBytes(const void* data, size_t size) {
    if (!data || size == 0) return std::string("(empty payload)");
    const u8* p = static_cast<const u8*>(data);
    const size_t previewN = (size < 32) ? size : 32;
    static const char* kHexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(previewN * 2);
    bool allZero = true;
    bool printable = true;
    for (size_t i = 0; i < previewN; ++i) {
        const u8 c = p[i];
        hex += kHexDigits[c >> 4];
        hex += kHexDigits[c & 0xF];
        if (c != 0) allZero = false;
        const bool isTextByte = (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
        if (!isTextByte) printable = false;
    }
    std::string kind;
    if (size >= 4 && p[0] == 'D' && p[1] == 'X' && p[2] == 'B' && p[3] == 'C') {
        kind = "looks like D3D12 DXIL .cso mirror (platform branching bug?)";
    } else if (allZero) {
        kind = "all-zero bytes";
    } else if (printable) {
        kind = "looks like text source (WGSL/GLSL/Metal) - a source path was fed to the bytecode loader";
    } else {
        kind = "unrecognized binary";
    }
    std::string out = "(" + kind + ") first32=0x" + hex + " (first " +
                      std::to_string(previewN) + " of " + std::to_string(size) + " bytes)";
    return out;
}

} // namespace detail

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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanShader] No SPIR-V data" << std::endl;
        return false;
    }
    // SPIR-V 必须按 4 字节对齐(size 必须是 4 的倍数)
    if (spirv_.size() % 4 != 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanShader] SPIR-V size " << spirv_.size()
                  << " not multiple of 4" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv_.size();
    ci.pCode = reinterpret_cast<const u32*>(spirv_.data());

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    if (vkCreateShaderModule(dev, &ci, nullptr, &module_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanShader] vkCreateShaderModule failed" << std::endl;
        // R7-WIN 双保险:绕过 createShaderImpl 预检的路径(Reload/直接
        // 构造)同样给出 entryPoint + 判类 + hex 预览,把 01379 定位到
        // 具体入口。纯日志,失败路径本来就失败,合法 .spv 不经过本分支。
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[VulkanShader] reject non-SPIR-V bytes: entryPoint=" << entryPoint_
            << " size=" << spirv_.size() << " "
            << detail::DescribeBadSpirvBytes(spirv_.data(), spirv_.size()) << std::endl;
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
