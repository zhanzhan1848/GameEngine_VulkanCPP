/**
 * @file RHIBatchRenderer.h
 * @brief RHI智能批渲染优化器
 * @details 提供渲染项智能分类、动态批处理优化和GPU实例化功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHIMath.h"
#include "RHIGeometry.h"

namespace primal::graphics::rhi {

// === 前向声明 ===
class RHIDeviceBase;
class RHICommandBuffer;
struct RenderItem;
struct BatchConfig;
struct RenderBatch;

// === 类型别名 ===
using PipelineHandle = u64;
using ResourceHandle = u64;
enum class DataFormat : u16;

/**
 * @brief 渲染项分类键
 * @details 用于将相似的渲染项归类到同一批次中
 */
struct RenderItemKey {
    PipelineHandle pipeline;           ///< 图形管线句柄
    ResourceHandle vertexBuffer;       ///< 顶点缓冲区
    ResourceHandle indexBuffer;        ///< 索引缓冲区
    ResourceHandle material;           ///< 材质资源
    u32 materialSlot;                  ///< 材质槽位
    u32 vertexStride;                  ///< 顶点步长
    DataFormat indexFormat;            ///< 索引格式
    u8 renderTarget;                   ///< 渲染目标索引
    
    /**
     * @brief 比较操作符（用于哈希表排序）
     */
    bool operator==(const RenderItemKey& other) const {
        return pipeline == other.pipeline &&
               vertexBuffer == other.vertexBuffer &&
               indexBuffer == other.indexBuffer &&
               material == other.material &&
               materialSlot == other.materialSlot &&
               vertexStride == other.vertexStride &&
               indexFormat == other.indexFormat &&
               renderTarget == other.renderTarget;
    }
    
    /**
     * @brief 小于操作符（用于排序）
     */
    bool operator<(const RenderItemKey& other) const {
        if (pipeline != other.pipeline) return pipeline < other.pipeline;
        if (vertexBuffer != other.vertexBuffer) return vertexBuffer < other.vertexBuffer;
        if (indexBuffer != other.indexBuffer) return indexBuffer < other.indexBuffer;
        if (material != other.material) return material < other.material;
        if (materialSlot != other.materialSlot) return materialSlot < other.materialSlot;
        if (vertexStride != other.vertexStride) return vertexStride < other.vertexStride;
        if (indexFormat != other.indexFormat) return indexFormat < other.indexFormat;
        return renderTarget < other.renderTarget;
    }
};

/**
 * @brief 渲染项哈希函数
 * @details 使用 MurmurHash3 算法对 RenderItemKey 结构体进行高效哈希计算
 */
struct RenderItemKeyHash {
    /**
     * @brief 哈希函数调用操作符
     * @param key 要哈希的渲染项键
     * @return 哈希值
     * @details 使用 MurmurHash3_x86_32 算法对整个 RenderItemKey 结构体进行哈希计算
     *          种子值固定为 42，确保哈希结果的一致性和可重现性
     */
    size_t operator()(const RenderItemKey& key) const {
        u32 hashResult = 0;
        static constexpr u32 HASH_SEED = 42; // 固定种子值确保一致性
        
        // 使用 MurmurHash3_x86_32 对整个结构体进行哈希
        primal::utl::MurmurHash3_x86_32(&key, sizeof(RenderItemKey), HASH_SEED, &hashResult);
        
        return static_cast<size_t>(hashResult);
    }
};

/**
 * @brief 渲染项描述符
 * @details 描述单个渲染项的所有必要信息
 */
struct RenderItem {
    RenderItemKey key;                  ///< 分类键
    math::m4x4 worldMatrix;          ///< 世界变换矩阵
    math::v4 color;                     ///< 颜色调制
    u32 startVertex;                    ///< 起始顶点
    u32 vertexCount;                    ///< 顶点数量
    u32 startIndex;                     ///< 起始索引
    u32 indexCount;                     ///< 索引数量
    u32 instanceCount;                  ///< 实例数量
    u32 startInstance;                  ///< 起始实例
    f32 depth;                          ///< 深度值（用于排序）
    u32 materialID;                     ///< 材质ID
    u8 visibilityMask;                  ///< 可见性掩码
    bool isInstanced;                   ///< 是否为实例化渲染
    
    RenderItem() : startVertex(0), vertexCount(0), startIndex(0), indexCount(0),
                   instanceCount(1), startInstance(0), depth(0.0f), materialID(0),
                   visibilityMask(0xFF), isInstanced(false) {
        worldMatrix = math::MatrixIdentity();
        color = math::v4{1.0f, 1.0f, 1.0f, 1.0f};
    }
};

/**
 * @brief 渲染批次
 * @details 包含相同分类键的多个渲染项
 */
struct RenderBatch {
    RenderItemKey key;                  ///< 批次分类键
    utl::vector<RenderItem> items;     ///< 渲染项列表
    u32 totalVertices;                  ///< 总顶点数
    u32 totalIndices;                   ///< 总索引数
    u32 totalInstances;                 ///< 总实例数
    f32 boundingRadius;                 ///< 包围半径
    bool needsSorting;                  ///< 是否需要排序
    bool isInstancedBatch;              ///< 是否为实例化批次
    
    RenderBatch() : totalVertices(0), totalIndices(0), totalInstances(0),
                    boundingRadius(0.0f), needsSorting(true), isInstancedBatch(false) {
        items.reserve(64);
    }
    
    /**
     * @brief 添加渲染项到批次
     * @param item 渲染项
     */
    void AddItem(const RenderItem& item) {
        items.push_back(item);
        totalVertices += item.vertexCount;
        totalIndices += item.indexCount;
        totalInstances += item.instanceCount;
        
        // 更新包围半径（简化计算）
        math::v3 translation{item.worldMatrix.columns[3][0], item.worldMatrix.columns[3][1], item.worldMatrix.columns[3][2]};
        f32 itemRadius = std::sqrt(math::dot(translation, translation));
        boundingRadius = std::max(boundingRadius, itemRadius);
        
        // 标记需要重新排序
        needsSorting = true;
    }
    
    /**
     * @brief 清空批次
     */
    void Clear() {
        items.clear();
        totalVertices = 0;
        totalIndices = 0;
        totalInstances = 0;
        boundingRadius = 0.0f;
        needsSorting = true;
    }
};

/**
 * @brief 批处理配置
 * @details 控制批处理优化的各种参数
 */
struct BatchConfig {
    u32 maxBatchSize;                   ///< 最大批次大小（渲染项数量）
    u32 maxVerticesPerBatch;            ///< 每批次最大顶点数
    u32 maxIndicesPerBatch;             ///< 每批次最大索引数
    u32 maxInstancesPerBatch;           ///< 每批次最大实例数
    f32 batchDistanceThreshold;         ///< 批处理距离阈值
    bool enableInstancing;              ///< 启用实例化
    bool enableDepthSorting;            ///< 启用深度排序
    bool enableFrustumCulling;          ///< 启用视锥剔除
    bool enableOcclusionCulling;        ///< 启用遮挡剔除
    
    BatchConfig() : maxBatchSize(256), maxVerticesPerBatch(65536),
                    maxIndicesPerBatch(65536), maxInstancesPerBatch(1024),
                    batchDistanceThreshold(100.0f), enableInstancing(true),
                    enableDepthSorting(true), enableFrustumCulling(true),
                    enableOcclusionCulling(false) {}
};

/**
 * @brief 批处理统计信息
 * @details 用于性能分析和调试
 */
struct BatchStats {
    u32 totalRenderItems;               ///< 总渲染项数
    u32 totalBatches;                   ///< 总批次数
    u32 instancedBatches;               ///< 实例化批次数
    u32 mergedBatches;                  ///< 合并的批次数
    u32 culledItems;                    ///< 剔除的渲染项数
    f32 averageBatchSize;               ///< 平均批次大小
    f32 batchingEfficiency;             ///< 批处理效率（0-1）
    f32 totalProcessingTime;             ///< 总处理时间（毫秒）
    u64 memoryUsage;                    ///< 内存使用量（字节）
    
    BatchStats() : totalRenderItems(0), totalBatches(0), instancedBatches(0),
                   mergedBatches(0), culledItems(0), averageBatchSize(0.0f),
                   batchingEfficiency(0.0f), totalProcessingTime(0.0), memoryUsage(0) {}
};

/**
 * @brief RHI智能批渲染优化器
 * @details 提供高性能的渲染项分类、批处理和实例化功能
 */
class RHIBatchRenderer {
public:
    /**
     * @brief 构造函数
     * @param device RHI设备引用
     * @param config 批处理配置
     */
    RHIBatchRenderer(RHIDeviceBase& device, const BatchConfig& config = BatchConfig{});
    
    /**
     * @brief 析构函数
     */
    ~RHIBatchRenderer();
    
    // === 核心功能接口 ===
    
    /**
     * @brief 初始化批渲染器
     * @return 是否成功
     */
    bool Initialize();
    
    /**
     * @brief 关闭批渲染器
     */
    void Shutdown();
    
    /**
     * @brief 添加渲染项
     * @param item 渲染项
     */
    void AddRenderItem(const RenderItem& item);
    
    /**
     * @brief 批量添加渲染项
     * @param items 渲染项数组
     * @param count 渲染项数量
     */
    void AddRenderItems(const RenderItem* items, u32 count);
    
    /**
     * @brief 清空所有渲染项
     */
    void ClearRenderItems();
    
    /**
     * @brief 执行批处理优化
     * @param viewMatrix 视图矩阵
     * @param projectionMatrix 投影矩阵
     * @return 批次数量
     */
    u32 ProcessBatches(const math::m4x4& viewMatrix, const math::m4x4& projectionMatrix);
    
    /**
     * @brief 提交批次到命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @return 提交的批次数量
     */
    u32 SubmitBatches(RHICommandBuffer* commandBuffer);
    
    // === 配置管理 ===
    
    /**
     * @brief 获取批处理配置
     * @return 批处理配置引用
     */
    const BatchConfig& GetConfig() const { return config_; }
    
    /**
     * @brief 设置批处理配置
     * @param config 新配置
     */
    void SetConfig(const BatchConfig& config) { config_ = config; }
    
    // === 统计信息 ===
    
    /**
     * @brief 获取统计信息
     * @return 统计信息
     */
    const BatchStats& GetStats() const { return stats_; }
    
    /**
     * @brief 重置统计信息
     */
    void ResetStats();
    
    // === 调试和诊断 ===
    
    /**
     * @brief 获取批次列表（只读）
     * @return 批次列表
     */
    const utl::vector<RenderBatch>& GetBatches() const { return batches_; }
    
    /**
     * @brief 打印统计信息
     */
    void PrintStats() const;
    
    /**
     * @brief 验证批处理结果
     * @return 是否有效
     */
    bool ValidateBatches() const;

private:
    // === 内部处理方法 ===
    
    /**
     * @brief 分类渲染项到批次
     */
    void ClassifyRenderItems();
    
    /**
     * @brief 优化批次（合并、分割等）
     */
    void OptimizeBatches();
    
    /**
     * @brief 执行视锥剔除
     * @param frustum 视锥
     */
    void PerformFrustumCulling(const Frustum& frustum);
    
    /**
     * @brief 执行深度排序
     */
    void PerformDepthSorting();
    
    /**
     * @brief 检查是否可以合并批次
     * @param batch1 第一个批次
     * @param batch2 第二个批次
     * @return 是否可以合并
     */
    bool CanMergeBatches(const RenderBatch& batch1, const RenderBatch& batch2) const;
    
    /**
     * @brief 合并两个批次
     * @param target 目标批次
     * @param source 源批次
     */
    void MergeBatches(RenderBatch& target, const RenderBatch& source);
    
    /**
     * @brief 分割过大的批次
     * @param batch 要分割的批次
     * @return 分割后的新批次数量
     */
    u32 SplitBatch(RenderBatch& batch);
    
    /**
     * @brief 创建实例化批次
     * @param batch 原始批次
     * @return 实例化批次列表
     */
    utl::vector<RenderBatch> CreateInstancedBatches(const RenderBatch& batch);
    
    /**
     * @brief 提交单个批次到命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @param batch 渲染批次
     */
    void SubmitBatch(RHICommandBuffer* commandBuffer, const RenderBatch& batch);
    
    /**
     * @brief 提交实例化批次到命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @param batch 实例化批次
     */
    void SubmitInstancedBatch(RHICommandBuffer* commandBuffer, const RenderBatch& batch);
    
    /**
     * @brief 更新统计信息
     */
    void UpdateStats();

private:
    RHIDeviceBase& device_;                          ///< RHI设备引用
    BatchConfig config_;                             ///< 批处理配置
    BatchStats stats_;                               ///< 统计信息
    
    utl::vector<RenderItem> renderItems_;            ///< 渲染项列表
    std::unordered_map<RenderItemKey, std::unique_ptr<RenderBatch>, RenderItemKeyHash> batchMap_;  ///< 批次映射表
    utl::vector<RenderBatch> batches_;               ///< 处理后的批次列表
    
    math::m4x4 viewMatrix_;                       ///< 当前视图矩阵
    math::m4x4 projectionMatrix_;                 ///< 当前投影矩阵
    Frustum currentFrustum_;                          ///< 当前视锥
    
    bool isInitialized_;                             ///< 是否已初始化
    u32 frameCounter_;                               ///< 帧计数器
    
    // 性能优化相关的临时缓冲区
    utl::vector<u32> tempIndices_;                    ///< 临时索引缓冲区
    utl::vector<math::m4x4> tempInstanceData_;    ///< 临时实例数据
    utl::vector<RenderBatch> tempBatches_;           ///< 临时批次缓冲区
};

} // namespace primal::graphics::rhi