#pragma once

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <cstring>

namespace primal::graphics::rhi {

/**
 * @brief GPU缓冲区组件
 * @details 负责管理GPU缓冲区的生命周期和状态，符合ECS架构
 */
struct GPUBufferComponent {
    ResourceHandle handle;               ///< GPU缓冲区资源句柄 (由 ResourceManager 管理生命周期)
    uint64_t size;                       ///< 缓冲区大小（字节）
    BufferType type;                     ///< 缓冲区类型（顶点/索引/常量等）
    GPUMemoryUsage usage;                ///< 内存使用模式（静态/动态/流式等）
    uint32_t bindFlags;                  ///< 绑定标志位 (BufferUsageFlags)
    void* mappedPointer;                 ///< 映射到CPU的指针（如果已映射，否则为nullptr）

    GPUBufferComponent() : handle(handles::INVALID_RESOURCE), size(0), type(BufferType::Unknown), usage(GPUMemoryUsage::Unknown), bindFlags(0), mappedPointer(nullptr) {}
    
    GPUBufferComponent(ResourceHandle inHandle, uint64_t inSize, BufferType inType, GPUMemoryUsage inUsage, uint32_t inBindFlags = 0) 
        : handle(inHandle), size(inSize), type(inType), usage(inUsage), bindFlags(inBindFlags), mappedPointer(nullptr) {}

    bool IsValid() const { return handle != handles::INVALID_RESOURCE && size > 0; }

    /**
     * @brief 创建GPU缓冲区
     * @param device RHI设备指针
     * @param debugName 调试名称
     * @return 是否创建成功
     */
    bool Create(RHIDeviceBase* device, const char* debugName = nullptr) {
        if (!device) return false;
        if (IsValid()) return true; // 已经创建

        BufferDesc desc;
        desc.size = size;
        desc.type = type;
        desc.usage = usage;
        desc.memoryUsage = usage; // 兼容字段
        desc.bindFlags = bindFlags;
        if (debugName) desc.name = debugName;

        handle = device->CreateBuffer(desc);
        return handle != handles::INVALID_RESOURCE;
    }

    /**
     * @brief 销毁GPU缓冲区
     * @param device RHI设备指针
     */
    void Destroy(RHIDeviceBase* device) {
        if (!device || !IsValid()) return;
        
        if (mappedPointer) {
            Unmap(device);
        }
        
        device->DestroyBuffer(handle);
        handle = handles::INVALID_RESOURCE;
        mappedPointer = nullptr;
    }

    /**
     * @brief 映射缓冲区到CPU内存
     * @param device RHI设备指针
     * @param offset 偏移量
     * @param mapSize 映射大小（0表示全部）
     * @return 映射后的指针
     */
    void* Map(RHIDeviceBase* device, uint64_t offset = 0, uint64_t mapSize = 0) {
        if (!device || !IsValid()) return nullptr;
        if (mappedPointer) return mappedPointer; // 已经映射

        // 如果mapSize为0，则映射整个缓冲区
        uint64_t sizeToMap = (mapSize == 0) ? size : mapSize;
        
        mappedPointer = device->MapBuffer(handle, offset, sizeToMap);
        return mappedPointer;
    }

    /**
     * @brief 取消映射
     * @param device RHI设备指针
     */
    void Unmap(RHIDeviceBase* device) {
        if (!device || !IsValid()) return;
        
        // 只有当已经映射时才取消映射
        if (mappedPointer) {
            device->UnmapBuffer(handle);
            mappedPointer = nullptr;
        }
    }
    
    /**
     * @brief 上传数据到缓冲区
     * @details 仅适用于CPU可见的缓冲区
     * @param device RHI设备指针
     * @param data 数据指针
     * @param dataSize 数据大小
     * @param offset 偏移量
     * @return 是否成功
     */
    bool Upload(RHIDeviceBase* device, const void* data, uint64_t dataSize, uint64_t offset = 0) {
        if (!device || !IsValid() || !data) return false;
        
        void* ptr = Map(device, offset, dataSize);
        if (!ptr) return false;
        
        std::memcpy(ptr, data, dataSize);
        
        Unmap(device);
        return true;
    }
};

} // namespace primal::graphics::rhi
