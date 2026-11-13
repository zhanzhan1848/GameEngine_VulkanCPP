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
        case rhi::RHIPlatform::Vulkan:
            return "Engine/Graphics/Vulkan/shaders/";
        // D3D12: DXIL .cso mirrors the Vulkan shader tree's relative paths
        // (build_d3d12_shaders.py output layout; spirv-cross rewrites every
        // entry point to "main").
        case rhi::RHIPlatform::D3D12:
            return "EngineTest/shaders/d3d12/";
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
        case rhi::RHIPlatform::Vulkan:
            return ".spv";
        case rhi::RHIPlatform::D3D12:
            return ".cso";
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

    // Resolve a repo-relative shader path (e.g. "Engine/Graphics/Metal/shaders/
    // BlurPass.metal") to an existing absolute/relative file path, independent
    // of the process CWD. Search order:
    //   1. $PRIMAL_SHADER_ROOT/<relPath>   (explicit deployment override)
    //   2. <relPath> as-is                 (CWD-relative, existing behavior)
    //   3. walk up from CWD                (launched from a build subdirectory)
    //   4. walk up from the executable dir (arbitrary CWD)
    // If nothing matches, relPath is returned unchanged so the caller's
    // "failed to open" error still names the expected location.
    static std::string ResolveShaderPath(const std::string& relPath);
};

} // namespace primal::graphics::utils
