#pragma once

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <string>

namespace primal::graphics::rhi {

/**
 * @brief 纹理组件
 * @details 负责管理纹理资源的生命周期和状态，符合ECS架构
 */
struct TextureComponent {
    ResourceHandle handle;           ///< 纹理资源句柄 (由 ResourceManager 管理生命周期)
    ResourceHandle defaultView;      ///< 默认纹理视图句柄 (可选)
    TextureDesc desc;                ///< 纹理描述符
    TextureViewDesc defaultViewDesc; ///< 默认视图描述符

    TextureComponent() 
        : handle(handles::INVALID_RESOURCE), 
          defaultView(handles::INVALID_RESOURCE) {}
    
    // 构造函数
    TextureComponent(const TextureDesc& inDesc, const char* inName = nullptr) 
        : handle(handles::INVALID_RESOURCE), 
          defaultView(handles::INVALID_RESOURCE), 
          desc(inDesc) {
        if (inName) desc.name = inName;
    }

    bool IsValid() const { return handle != handles::INVALID_RESOURCE; }

    /**
     * @brief 创建纹理
     * @param device RHI设备指针
     * @param createDefaultView 是否创建默认视图（全资源视图）
     * @return 是否创建成功
     */
    bool Create(RHIDeviceBase* device, bool createDefaultView = true);

    /**
     * @brief 销毁纹理
     * @param device RHI设备指针
     */
    void Destroy(RHIDeviceBase* device);

    /**
     * @brief 创建纹理视图
     * @param device RHI设备指针
     * @param viewDesc 视图描述符
     * @return 视图句柄，失败返回 INVALID_RESOURCE
     */
    ResourceHandle CreateView(RHIDeviceBase* device, const TextureViewDesc& viewDesc);
    
    /**
     * @brief 获取纹理尺寸
     * @return u32v3 (width, height, depth)
     */
    math::u32v3 GetSize() const { return desc.size; }
    
    /**
     * @brief 获取纹理格式
     * @return 数据格式
     */
    DataFormat GetFormat() const { return desc.format; }
};

} // namespace primal::graphics::rhi
