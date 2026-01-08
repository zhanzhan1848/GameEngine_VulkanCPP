/**
 * @file MetalQuery.h
 * @brief Metal 查询堆实现
 * @details 封装 MTLCounterSampleBuffer 实现时间戳和统计查询
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief Metal 查询类型
 */
enum class MetalQueryType {
    Timestamp,  ///< 时间戳查询
    Occlusion,  ///< 遮挡查询 (使用 MTLVisibilityResultMode)
    Statistics  ///< 统计查询 (Metal 部分支持)
};

/**
 * @brief Metal 查询堆
 * @details 封装 MTLCounterSampleBuffer
 */
class MetalQueryPool {
public:
    /**
     * @brief 构造函数
     * @param device Metal 设备指针
     * @param type 查询类型
     * @param count 查询数量
     */
    MetalQueryPool(MTL::Device* device, MetalQueryType type, uint32_t count);

    /**
     * @brief 析构函数
     */
    ~MetalQueryPool();

    /**
     * @brief 获取原生 CounterSampleBuffer 对象
     */
    MTL::CounterSampleBuffer* GetNativeBuffer() const { return buffer_; }
    
    /**
     * @brief 获取原生 Buffer 对象 (用于遮挡查询)
     */
    MTL::Buffer* GetVisibilityBuffer() const { return visibilityBuffer_; }

    /**
     * @brief 获取查询类型
     */
    MetalQueryType GetType() const { return type_; }

    /**
     * @brief 获取查询数量
     */
    uint32_t GetCount() const { return count_; }

    /**
     * @brief 获取查询结果
     * @param firstQuery 起始查询索引
     * @param queryCount 查询数量
     * @param data 输出数据指针
     * @param stride 数据步长
     * @return 是否获取成功
     */
    bool GetResults(uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride);

private:
    MTL::CounterSampleBuffer* buffer_{nullptr}; ///< 计数器采样缓冲区 (用于 Timestamp)
    MTL::Buffer* visibilityBuffer_{nullptr};    ///< 可见性结果缓冲区 (用于 Occlusion)
    MetalQueryType type_;                       ///< 查询类型
    uint32_t count_;                            ///< 查询数量
};

} // namespace primal::graphics::rhi
