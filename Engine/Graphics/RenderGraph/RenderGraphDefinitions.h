#pragma once

#include "CommonHeaders.h"
#include <cstdint>
#include <string>
#include <functional>
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIResource.h"

namespace primal::graphics::rendergraph {

using namespace primal::graphics::rhi;

// 资源句柄
struct RGResourceHandle {
    uint32_t index = 0;
    uint32_t version = 0;

    bool IsValid() const { return index != 0; }
    bool operator==(const RGResourceHandle& other) const { return index == other.index && version == other.version; }
    bool operator!=(const RGResourceHandle& other) const { return !(*this == other); }
};

// 无效句柄
static const RGResourceHandle kInvalidRGResourceHandle = {0, 0};

// Pass 类型
enum class RGPassType {
    Graphics,
    Compute,
    Copy,
    AsyncCompute,
    AsyncCopy
};

// Pass 分类
enum class RGPassCategory {
    None,
    Visibility,     // Z-Prepass / Visibility Buffer
    Depth,          // Shadow Maps
    Main,           // GBuffer / Forward Lighting
    Lighting,       // Deferred Lighting / Global Illumination (SSAO, SSGI)
    PostProcess,    // Bloom, ToneMapping, Color Grading
    UI,             // User Interface
    Present         // Final Blit
};

// 资源访问类型
enum class RGAccessType {
    Read,
    Write,
    ReadWrite
};

// 资源标志
enum class RGResourceFlags {
    None = 0,
    Imported = 1 << 0,  // 外部导入资源
    Output = 1 << 1,    // 图的输出资源 (不被剔除)
    Transient = 1 << 2  // 临时资源 (自动管理生命周期)
};

// 资源类型
enum class RGResourceType {
    Unknown,
    Texture,
    Buffer
};

inline RGResourceFlags operator|(RGResourceFlags a, RGResourceFlags b) {
    return static_cast<RGResourceFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool HasFlag(RGResourceFlags flags, RGResourceFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

struct RGAttachmentDesc {
    RGResourceHandle texture = kInvalidRGResourceHandle;
    uint32_t level = 0;
    uint32_t slice = 0;
    rhi::LoadAction loadOp = rhi::LoadAction::DontCare;
    rhi::StoreAction storeOp = rhi::StoreAction::DontCare;
    rhi::ClearValue clearColor = {0, 0, 0, 0};
    
    // Depth/Stencil specific
    rhi::LoadAction depthLoadOp = rhi::LoadAction::DontCare;
    rhi::StoreAction depthStoreOp = rhi::StoreAction::DontCare;
    rhi::LoadAction stencilLoadOp = rhi::LoadAction::DontCare;
    rhi::StoreAction stencilStoreOp = rhi::StoreAction::DontCare;
    float clearDepth = 1.0f;
    uint8_t clearStencil = 0;
};

struct RGRenderPassDesc {
    std::vector<RGAttachmentDesc> colors;
    RGAttachmentDesc depthStencil;
    uint32_t renderTargetArrayLength = 0; // Default to 0 (No Layered Rendering)
};

} // namespace primal::graphics::rendergraph

namespace std {
    template<>
    struct hash<primal::graphics::rendergraph::RGResourceHandle> {
        size_t operator()(const primal::graphics::rendergraph::RGResourceHandle& handle) const {
            // Combine index and version
            // A simple hash combination
            return std::hash<uint32_t>()(handle.index) ^ (std::hash<uint32_t>()(handle.version) << 1);
        }
    };
}
