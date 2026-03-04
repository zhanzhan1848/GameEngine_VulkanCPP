/**
 * @file RenderLayerComponent.h
 * @brief 渲染层级与视口配置组件
 * @details 管理实体的渲染层级掩码、优先级以及视口/裁剪区域配置
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-30
 * @version 0.1.0
 */

#pragma once

#include "../Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief 渲染层级组件
 * @details 该组件用于ECS架构，既可用于渲染对象(定义其属于哪些层)，
 *          也可用于相机/渲染源(定义其渲染哪些层以及视口配置)
 */
struct RenderLayerComponent {
    // === 基础数据 ===
    u32 layerMask;          ///< 层级掩码 (每一位代表一个层，共32层)
    s32 priority;            ///< 渲染排序优先级 (越小越先渲染，默认为0)
    
    // === 视口与裁剪配置 (通常用于相机实体) ===
    ViewportDesc viewport;       ///< 视口配置
    Rect scissor;                ///< 裁剪区域
    
    bool useCustomViewport;      ///< 是否使用自定义视口
    bool useCustomScissor;       ///< 是否使用自定义裁剪

    // === 构造函数 ===
    RenderLayerComponent();
    
    // === 辅助方法 ===
    
    /**
     * @brief 设置所属层级（会清除其他层级）
     * @param layerIndex 层级索引 (0-31)
     */
    void SetLayer(u8 layerIndex);
    
    /**
     * @brief 启用特定层级
     * @param layerIndex 层级索引 (0-31)
     */
    void EnableLayer(u8 layerIndex);
    
    /**
     * @brief 禁用特定层级
     * @param layerIndex 层级索引 (0-31)
     */
    void DisableLayer(u8 layerIndex);
    
    /**
     * @brief 检查是否包含特定层级
     * @param layerIndex 层级索引 (0-31)
     * @return 是否包含
     */
    bool HasLayer(u8 layerIndex) const;
    
    /**
     * @brief 检查是否与指定掩码有重叠
     * @param mask 待检查的掩码
     * @return 是否重叠
     */
    bool Match(u32 mask) const;
};

} // namespace primal::graphics::rhi
