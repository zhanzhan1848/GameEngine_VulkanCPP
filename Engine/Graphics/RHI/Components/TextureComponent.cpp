#include "TextureComponent.h"
#include <cassert>

namespace primal::graphics::rhi {

bool TextureComponent::Create(RHIDeviceBase* device, bool createDefaultView) {
    if (!device) return false;
    if (IsValid()) return true; // 已经创建

    // 创建纹理
    handle = device->CreateTexture(desc);
    if (handle == handles::INVALID_RESOURCE) {
        return false;
    }

    // 创建默认视图
    if (createDefaultView) {
        defaultViewDesc.texture = handle;
        defaultViewDesc.viewType = desc.type;
        defaultViewDesc.format = desc.format;
        defaultViewDesc.mostDetailedMip = 0;
        defaultViewDesc.mipCount = desc.mipLevels;
        defaultViewDesc.firstArraySlice = 0;
        defaultViewDesc.arraySize = desc.arraySize;
        
        defaultView = device->CreateTextureView(defaultViewDesc);
        // 注意：视图创建失败可能不被视为组件创建失败，但应该记录或处理
        // 如果视图必须存在，则返回 false 并销毁纹理
    }

    return true;
}

void TextureComponent::Destroy(RHIDeviceBase* device) {
    if (!device || !IsValid()) return;
    
    // 销毁默认视图
    if (defaultView != handles::INVALID_RESOURCE) {
        device->DestroyTexture(defaultView); // 这里假设 DestroyTexture 也可以用于视图，或者有 DestroyTextureView
        // 查看 RHIDevice 发现 DestroyTexture 是通用的资源销毁，还是专门针对纹理？
        // 实际上 ResourceHandle 统一管理，RHIDevice::DestroyTexture 接受 ResourceHandle
        // 但是通常 API 会区分。让我们检查 RHIDevice::DestroyTexture 的定义。
        // MockDevice 中是 void DestroyTexture(ResourceHandle) override {}
        // 这意味着它接受 ResourceHandle。
        defaultView = handles::INVALID_RESOURCE;
    }
    
    // 销毁纹理
    device->DestroyTexture(handle);
    handle = handles::INVALID_RESOURCE;
}

ResourceHandle TextureComponent::CreateView(RHIDeviceBase* device, const TextureViewDesc& viewDesc) {
    if (!device || !IsValid()) return handles::INVALID_RESOURCE;
    
    // 确保视图指向当前纹理
    TextureViewDesc finalDesc = viewDesc;
    finalDesc.texture = handle;
    
    return device->CreateTextureView(finalDesc);
}

} // namespace primal::graphics::rhi
