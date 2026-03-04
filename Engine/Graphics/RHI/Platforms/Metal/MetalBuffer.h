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
     * @brief 设置堆分配信息
     * @param heap Metal堆对象
     * @param offset 堆内偏移
     * @param pool 来源内存池（用于释放）
     * @param handle 内存池分配句柄
     */
    void SetHeapAllocation(MTL::Heap* heap, u64 offset, class RHIAdaptiveMemoryPool* pool, u32 handle);

    /**
     * @brief 获取Metal缓冲区对象
     * @return MTLBuffer指针
     */
    MTL::Buffer* GetNativeBuffer() const { return mtlBuffer_; }

protected:
    // === RHIResource 接口实现 ===
    
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    MTL::Buffer* mtlBuffer_{nullptr};   ///< Metal缓冲区对象
    
    // 堆分配信息
    MTL::Heap* heap_{nullptr};          ///< 来源堆
    u64 heapOffset_{0};            ///< 堆内偏移
    class RHIAdaptiveMemoryPool* pool_{nullptr}; ///< 来源内存池
    u32 poolHandle_{0};            ///< 内存池分配句柄

    // 辅助函数
    MTL::ResourceOptions getResourceOptions() const;
};

} // namespace primal::graphics::rhi
