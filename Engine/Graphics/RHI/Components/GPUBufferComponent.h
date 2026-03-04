#pragma once

#include "Engine/Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief GPU缓冲区组件
 * @details 存储用于渲染的顶点缓冲和索引缓冲信息
 */
struct GPUBufferComponent {
    // 顶点缓冲信息
    ResourceHandle vertexBuffer = handles::INVALID_RESOURCE;
    u64 offset = 0;
    u32 vertexCount = 0;

    // 索引缓冲信息
    ResourceHandle indexBuffer = handles::INVALID_RESOURCE;
    u64 indexOffset = 0;
    u32 indexCount = 0;
    DataFormat indexType = DataFormat::R32_UInt;

    // 辅助方法：是否有效
    bool IsValid() const {
        return vertexBuffer != handles::INVALID_RESOURCE && vertexCount > 0;
    }
    
    bool HasIndexBuffer() const {
        return indexBuffer != handles::INVALID_RESOURCE && indexCount > 0;
    }
};

} // namespace primal::graphics::rhi
