#pragma once

#include "../../Core/RHIResource.h"
#include "MetalCommon.h"

namespace primal::graphics::rhi {

class MetalDevice;

/**
 * @brief Metal纹理实现
 * @details 封装MTLTexture对象，实现RHIResource接口
 */
class MetalTexture : public RHIResource {
    friend class MetalDevice;
public:
    /**
     * @brief 构造函数
     * @param device Metal设备引用
     * @param desc 纹理描述符
     */
    MetalTexture(MetalDevice& device, const TextureDesc& desc);

    // 移动构造函数
    MetalTexture(MetalTexture&& other) noexcept;
    // 移动赋值运算符
    MetalTexture& operator=(MetalTexture&& other) noexcept;

    // 禁用拷贝
    MetalTexture(const MetalTexture&) = delete;
    MetalTexture& operator=(const MetalTexture&) = delete;

    /**
     * @brief 析构函数
     */
    virtual ~MetalTexture();

    /**
     * @brief 初始化资源
     * @return 初始化是否成功
     */
    bool Initialize() override;

    /**
     * @brief 获取Metal纹理对象
     * @return MTLTexture指针
     */
    MTL::Texture* GetNativeTexture() const { return mtlTexture_; }

    /**
     * @brief 设置原生Metal纹理对象
     * @param texture MTLTexture指针
     * @details 用于交换链等需要动态替换底层纹理的场景。
     *          会自动处理引用计数：release旧纹理，retain新纹理。
     */
    void SetNativeTexture(MTL::Texture* texture) { 
        if (mtlTexture_ != texture) {
            if (mtlTexture_) mtlTexture_->release();
            mtlTexture_ = texture;
            if (mtlTexture_) mtlTexture_->retain();
        }
    }

protected:
    // === RHIResource 接口实现 ===
    
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    MTL::Texture* mtlTexture_{nullptr};   ///< Metal纹理对象
    TextureDesc textureDesc_;             ///< 纹理描述符 (Store copy or reference? RHIResource stores ResourceDesc base)
    // RHIResource stores desc_ which is ResourceDesc. TextureDesc contains more info.
    // RHIResource::desc_ is ResourceDesc. TextureDesc has extra fields like dimensions.
    // We should probably store TextureDesc.
    // Wait, RHIResource constructor takes ResourceDesc. TextureDesc does NOT inherit from ResourceDesc in RHITypes.h?
    // Let's check RHITypes.h again.
};

} // namespace primal::graphics::rhi
