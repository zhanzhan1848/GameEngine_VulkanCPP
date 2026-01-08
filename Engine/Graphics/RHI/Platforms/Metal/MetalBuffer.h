#pragma once

#include "../../Core/RHIResource.h"
#include "MetalCommon.h"

namespace primal::graphics::rhi {

class MetalDevice;

/**
 * @brief Metal缓冲区实现
 * @details 封装MTLBuffer对象，实现RHIResource接口
 */
class MetalBuffer : public RHIResource {
    friend class MetalDevice;
public:
    /**
     * @brief 构造函数
     * @param device Metal设备引用
     * @param desc 缓冲区描述符
     */
    MetalBuffer(MetalDevice& device, const BufferDesc& desc);

    // 移动构造函数
    MetalBuffer(MetalBuffer&& other) noexcept;
    // 移动赋值运算符
    MetalBuffer& operator=(MetalBuffer&& other) noexcept;

    // 禁用拷贝
    MetalBuffer(const MetalBuffer&) = delete;
    MetalBuffer& operator=(const MetalBuffer&) = delete;

    /**
     * @brief 析构函数
     */
    virtual ~MetalBuffer();

    /**
     * @brief 初始化资源
     * @return 初始化是否成功
     */
    bool Initialize() override;

    /**
     * @brief 获取Metal缓冲区对象
     * @return MTLBuffer指针
     */
    MTL::Buffer* GetNativeBuffer() const { return mtlBuffer_; }

protected:
    // === RHIResource 接口实现 ===
    
    void destroyImpl() override;
    void* mapImpl(uint64_t offset, uint64_t size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, uint64_t size, uint64_t offset) override;

private:
    MTL::Buffer* mtlBuffer_{nullptr};   ///< Metal缓冲区对象
    
    // 辅助函数
    MTL::ResourceOptions getResourceOptions() const;
};

} // namespace primal::graphics::rhi
