# 渲染层级与视口配置组件设计方案

## 1. 概述
`RenderLayerComponent` 是一个 ECS 组件，用于管理实体的渲染层级（Layer）以及视口（Viewport）和裁剪（Scissor）配置。
该组件具有双重职责：
1. **对于渲染对象（Renderables）**：定义对象属于哪些渲染层（例如：不透明层、透明层、UI层、高亮层等）。
2. **对于相机或渲染源（Cameras/RenderPasses）**：定义该相机渲染哪些层，以及使用什么样的视口和裁剪区域。

## 2. 数据结构

文件路径：`Engine/Graphics/RHI/Components/RenderLayerComponent.h`

```cpp
namespace primal::graphics::rhi {

struct RenderLayerComponent {
    // === 基础数据 ===
    uint32_t layerMask;          ///< 层级掩码 (每一位代表一个层，共32层)
    int32_t priority;            ///< 渲染排序优先级 (越小越先渲染，默认为0)
    
    // === 视口与裁剪配置 (通常用于相机实体) ===
    ViewportDesc viewport;       ///< 视口配置
    Rect scissor;                ///< 裁剪区域
    
    bool useCustomViewport;      ///< 是否使用自定义视口 (如果为false，则使用默认全屏或RenderPass默认设置)
    bool useCustomScissor;       ///< 是否使用自定义裁剪 (如果为false，则默认与视口一致)

    // === 构造函数 ===
    RenderLayerComponent();
    
    // === 辅助方法 ===
    
    /**
     * @brief 设置所属层级（会清除其他层级）
     * @param layerIndex 层级索引 (0-31)
     */
    void SetLayer(uint8_t layerIndex);
    
    /**
     * @brief 启用特定层级
     * @param layerIndex 层级索引 (0-31)
     */
    void EnableLayer(uint8_t layerIndex);
    
    /**
     * @brief 禁用特定层级
     * @param layerIndex 层级索引 (0-31)
     */
    void DisableLayer(uint8_t layerIndex);
    
    /**
     * @brief 检查是否包含特定层级
     * @param layerIndex 层级索引 (0-31)
     * @return 是否包含
     */
    bool HasLayer(uint8_t layerIndex) const;
    
    /**
     * @brief 检查是否与指定掩码有重叠
     * @param mask 待检查的掩码
     * @return 是否重叠
     */
    bool Match(uint32_t mask) const;
};

} // namespace primal::graphics::rhi
```

## 3. 实现细节

- `layerMask` 默认为 1 (Layer 0)。
- `priority` 默认为 0。
- `useCustomViewport` 和 `useCustomScissor` 默认为 `false`。
- 提供 helper 函数方便操作位掩码。

## 4. 与现有系统集成
- **RenderSystem**: 在构建 DrawPacket 或进行剔除时，使用 Camera 的 `RenderLayerComponent` (如果有) 的 `layerMask` 与 RenderObject 的 `RenderLayerComponent` (如果有) 的 `layerMask` 进行按位与操作 (`&`)。如果结果非零，则表示该对象对该相机可见。
- **RenderView**: `RenderView` 将存储 Viewport 和 Scissor 信息，这些信息可能来自 `RenderLayerComponent`。

## 5. 测试计划
- 创建 `RenderLayerComponent` 实例。
- 测试 `SetLayer`, `EnableLayer`, `DisableLayer` 是否正确修改 `layerMask`。
- 测试 `Match` 函数逻辑。
- 测试 Viewport 和 Scissor 数据的存取。
