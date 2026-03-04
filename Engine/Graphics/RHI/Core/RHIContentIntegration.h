/**
 * @file RHIContentIntegration.h
 * @brief RHI确定性预取系统与Content系统的集成
 * @details 提供ContentToEngine.cpp的扩展接口，支持确定性预取功能
 * 
 * @author Game Engine Team
 * @date 2025-01-04
 */

#pragma once

#include "RHIDeterministicPrefetch.h"
#include "Engine/Content/ContentToEngine.h"
#include <unordered_map>
#include <memory>

namespace primal::graphics::rhi {

// 前向声明
class RHIDeterministicPrefetchManager;

/**
 * @brief LOD预取信息
 */
struct LODPrefetchInfo {
    u32 lodLevel;                    // LOD级别
    u32 frameOffset;                 // 预取帧偏移
    u32 priority;                    // 预取优先级
    f32 confidence;                   // 预取置信度
    u64 estimatedSize;               // 预估大小(字节)
    bool isScheduled;                // 是否已调度
};

/**
 * @brief 着色器预取信息
 */
struct ShaderPrefetchInfo {
    utl::vector<std::string> shaderNames; // 着色器名称列表
    u32 frameOffset;                 // 预取帧偏移
    u32 priority;                    // 预取优先级
    f32 confidence;                   // 预取置信度
    u64 estimatedSize;               // 预估大小(字节)
    bool isScheduled;                // 是否已调度
};

/**
 * @brief 增强的几何体层次流
 * @details 继承自原有的geometry_hierarchy_stream，添加确定性预取功能
 */
class EnhancedGeometryHierarchyStream : public content::geometry_hierarchy_stream {
private:
    std::unique_ptr<RHIDeterministicPrefetchManager> prefetchManager_;
    std::unordered_map<u64, LODPrefetchInfo> lodPrefetchMap_;
    u64 geometryId_;
    bool prefetchEnabled_;
    
public:
    /**
     * @brief 构造函数
     * @param geometryId 几何体ID
     * @param prefetchManager 预取管理器
     */
    EnhancedGeometryHierarchyStream(
        u64 geometryId, 
        std::shared_ptr<RHIDeterministicPrefetchManager> prefetchManager);
    
    /**
     * @brief 析构函数
     */
    virtual ~EnhancedGeometryHierarchyStream() = default;
    
    // 重写基类方法
    bool get_geometry_hierarchy_buffer_size(u32 level, u64* size) override;
    bool get_geometry_hierarchy_buffer(u32 level, void* buffer, u64 bufferSize) override;
    bool set_geometry_hierarchy_buffer(u32 level, const void* buffer, u64 bufferSize) override;
    
    /**
     * @brief 确定性预取LOD
     * @param targetLevel 目标LOD级别
     * @param frameData 当前帧数据
     * @return 是否成功调度预取
     */
    bool DeterministicPrefetchLOD(u32 targetLevel, const FrameData& frameData);
    
    /**
     * @brief 检查LOD是否已在内存中
     * @param level LOD级别
     * @return 是否在内存中
     */
    bool IsLODInMemory(u32 level) const;
    
    /**
     * @brief 调度异步加载
     * @param level LOD级别
     * @param priority 加载优先级
     * @return 是否成功调度
     */
    bool ScheduleAsyncLoad(u32 level, u32 priority);
    
    /**
     * @brief 基于模式预测LOD
     * @param frameData 当前帧数据
     * @return 预测的LOD级别
     */
    u32 PredictLODByPattern(const FrameData& frameData);
    
    /**
     * @brief 计算预取帧偏移
     * @param resourceType 资源类型
     * @param hardwareClass 硬件等级
     * @return 帧偏移
     */
    u32 CalculatePrefetchFrameOffset(ResourceType resourceType, HardwareClass hardwareClass);
    
    /**
     * @brief 检查是否有内存预算加载LOD
     * @param level LOD级别
     * @return 是否有预算
     */
    bool HasMemoryBudgetForLOD(u32 level);
    
    /**
     * @brief 计算LOD优先级
     * @param level LOD级别
     * @param frameData 当前帧数据
     * @return 优先级
     */
    u32 CalculateLODPriority(u32 level, const FrameData& frameData);
    
    /**
     * @brief 估算LOD大小
     * @param level LOD级别
     * @return 估算大小(字节)
     */
    u64 EstimateLODSize(u32 level);
    
    /**
     * @brief 启用/禁用预取功能
     * @param enabled 是否启用
     */
    void SetPrefetchEnabled(bool enabled) { prefetchEnabled_ = enabled; }
    
    /**
     * @brief 获取预取信息
     * @param level LOD级别
     * @return 预取信息
     */
    const LODPrefetchInfo* GetLODPrefetchInfo(u32 level) const;
    
private:
    /**
     * @brief 初始化LOD模式表
     */
    void InitializeLODPatternTable();
    
    /**
     * @brief 更新LOD预取映射
     * @param level LOD级别
     * @param info 预取信息
     */
    void UpdateLODPrefetchMap(u32 level, const LODPrefetchInfo& info);
};

/**
 * @brief 增强的着色器组管理器
 * @details 管理着色器的确定性预取
 */
class EnhancedShaderGroupManager {
private:
    std::shared_ptr<RHIDeterministicPrefetchManager> prefetchManager_;
    std::unordered_map<std::string, ShaderPrefetchInfo> shaderPrefetchMap_;
    bool prefetchEnabled_;
    
public:
    /**
     * @brief 构造函数
     * @param prefetchManager 预取管理器
     */
    explicit EnhancedShaderGroupManager(
        std::shared_ptr<RHIDeterministicPrefetchManager> prefetchManager);
    
    /**
     * @brief 析构函数
     */
    virtual ~EnhancedShaderGroupManager() = default;
    
    /**
     * @brief 确定性预取着色器
     * @param shaderNames 着色器名称列表
     * @param frameData 当前帧数据
     * @return 是否成功调度预取
     */
    bool DeterministicPrefetchShaders(
        const utl::vector<std::string>& shaderNames,
        const FrameData& frameData);
    
    /**
     * @brief 检查着色器是否已在内存中
     * @param shaderName 着色器名称
     * @return 是否在内存中
     */
    bool IsShaderInMemory(const std::string& shaderName) const;
    
    /**
     * @brief 基于场景预测着色器
     * @param sceneType 场景类型
     * @return 预测的着色器列表
     */
    utl::vector<std::string> PredictShadersByScene(SceneType sceneType);
    
    /**
     * @brief 计算着色器优先级
     * @param shaderName 着色器名称
     * @param frameData 当前帧数据
     * @return 优先级
     */
    u32 CalculateShaderPriority(const std::string& shaderName, const FrameData& frameData);
    
    /**
     * @brief 估算着色器大小
     * @param shaderName 着色器名称
     * @return 估算大小(字节)
     */
    u64 EstimateShaderSize(const std::string& shaderName);
    
    /**
     * @brief 启用/禁用预取功能
     * @param enabled 是否启用
     */
    void SetPrefetchEnabled(bool enabled) { prefetchEnabled_ = enabled; }
    
    /**
     * @brief 获取着色器预取信息
     * @param shaderName 着色器名称
     * @return 预取信息
     */
    const ShaderPrefetchInfo* GetShaderPrefetchInfo(const std::string& shaderName) const;
    
private:
    /**
     * @brief 初始化着色器模式表
     */
    void InitializeShaderPatternTable();
    
    /**
     * @brief 更新着色器预取映射
     * @param shaderName 着色器名称
     * @param info 预取信息
     */
    void UpdateShaderPrefetchMap(const std::string& shaderName, const ShaderPrefetchInfo& info);
};

/**
 * @brief Content集成管理器
 * @details 协调Content系统与RHI预取系统的集成
 */
class ContentIntegrationManager {
private:
    std::shared_ptr<RHIDeterministicPrefetchManager> prefetchManager_;
    std::unordered_map<u64, std::shared_ptr<EnhancedGeometryHierarchyStream>> geometryStreams_;
    std::unique_ptr<EnhancedShaderGroupManager> shaderManager_;
    PrefetchConfiguration config_;
    bool isInitialized_;
    
public:
    /**
     * @brief 构造函数
     * @param config 预取配置
     */
    explicit ContentIntegrationManager(const PrefetchConfiguration& config = {});
    
    /**
     * @brief 析构函数
     */
    virtual ~ContentIntegrationManager() = default;
    
    /**
     * @brief 初始化集成管理器
     * @return 是否成功初始化
     */
    bool Initialize();
    
    /**
     * @brief 更新集成管理器
     * @param frameData 当前帧数据
     */
    void Update(const FrameData& frameData);
    
    /**
     * @brief 关闭集成管理器
     */
    void Shutdown();
    
    /**
     * @brief 创建或获取增强的几何体流
     * @param geometryId 几何体ID
     * @return 几何体流指针
     */
    std::shared_ptr<EnhancedGeometryHierarchyStream> GetOrCreateGeometryStream(u64 geometryId);
    
    /**
     * @brief 获取着色器管理器
     * @return 着色器管理器指针
     */
    EnhancedShaderGroupManager* GetShaderManager() const { return shaderManager_.get(); }
    
    /**
     * @brief 处理几何体访问事件
     * @param geometryId 几何体ID
     * @param lodLevel LOD级别
     * @param frameData 当前帧数据
     */
    void OnGeometryAccess(u64 geometryId, u32 lodLevel, const FrameData& frameData);
    
    /**
     * @brief 处理着色器访问事件
     * @param shaderName 着色器名称
     * @param frameData 当前帧数据
     */
    void OnShaderAccess(const std::string& shaderName, const FrameData& frameData);
    
    /**
     * @brief 设置场景类型
     * @param sceneType 场景类型
     */
    void SetSceneType(SceneType sceneType);
    
    /**
     * @brief 获取预取统计信息
     * @return 统计信息
     */
    deterministic_prefetch::IDeterministicPrefetchSystem::PerformanceStats GetPrefetchStats() const;
    
    /**
     * @brief 检查系统健康状态
     * @return 是否健康
     */
    bool IsHealthy() const;
    
    /**
     * @brief 更新配置
     * @param config 新配置
     */
    void UpdateConfiguration(const PrefetchConfiguration& config);
    
private:
    /**
     * @brief 初始化预取管理器
     * @return 是否成功初始化
     */
    bool InitializePrefetchManager();
    
    /**
     * @brief 初始化着色器管理器
     */
    void InitializeShaderManager();
    
    /**
     * @brief 清理过期的几何体流
     */
    void CleanupExpiredGeometryStreams();
};

/**
 * @brief Content集成工厂函数
 * @details 创建Content集成管理器实例
 * @param config 预取配置
 * @return 集成管理器指针
 */
std::unique_ptr<ContentIntegrationManager> CreateContentIntegrationManager(
    const PrefetchConfiguration& config = {});

/**
 * @brief Content集成工具函数
 */
namespace content_integration {

/**
 * @brief 创建增强的几何体流
 * @param geometryId 几何体ID
 * @param integrationManager 集成管理器
 * @return 几何体流指针
 */
std::shared_ptr<EnhancedGeometryHierarchyStream> CreateEnhancedGeometryStream(
    u64 geometryId,
    ContentIntegrationManager* integrationManager);

/**
 * @brief 注册Content访问回调
 * @param integrationManager 集成管理器
 * @return 是否成功注册
 */
bool RegisterContentAccessCallbacks(ContentIntegrationManager* integrationManager);

/**
 * @brief 配置Content系统预取参数
 * @param integrationManager 集成管理器
 * @param maxConcurrentLoads 最大并发加载数
 * @param memoryLimitMB 内存限制(MB)
 * @return 是否成功配置
 */
bool ConfigureContentPrefetchLimits(
    ContentIntegrationManager* integrationManager,
    u32 maxConcurrentLoads,
    u64 memoryLimitMB);

} // namespace content_integration

} // namespace primal::graphics::rhi