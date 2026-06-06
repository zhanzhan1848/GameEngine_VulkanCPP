#pragma once

#include <string>
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::utils {

class ShaderRegistry {
public:
    static std::string GetShaderBaseDir(rhi::RHIPlatform platform) {
        switch (platform) {
        case rhi::RHIPlatform::Metal:
            return "Engine/Graphics/Metal/shaders/";
        case rhi::RHIPlatform::Dawn:
            return "Engine/Graphics/Dawn/shaders/";
        default:
            return "Engine/Graphics/Metal/shaders/";
        }
    }

    static std::string GetShaderPath(rhi::RHIPlatform platform, const std::string& shaderName) {
        return GetShaderBaseDir(platform) + shaderName + GetShaderExtension(platform);
    }

    static std::string GetShaderExtension(rhi::RHIPlatform platform) {
        switch (platform) {
        case rhi::RHIPlatform::Metal:
            return ".metal";
        case rhi::RHIPlatform::Dawn:
            return ".wgsl";
        default:
            return ".metal";
        }
    }

    static std::string GetLumenShaderPath(rhi::RHIPlatform platform, const std::string& shaderName) {
        return GetShaderBaseDir(platform) + "Lumen/" + shaderName + GetShaderExtension(platform);
    }

    static std::string GetNaniteShaderPath(rhi::RHIPlatform platform, const std::string& shaderName) {
        return GetShaderBaseDir(platform) + "Nanite/" + shaderName + GetShaderExtension(platform);
    }
};

} // namespace primal::graphics::utils
