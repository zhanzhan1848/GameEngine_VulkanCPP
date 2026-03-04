# 材质组件设计方案

## 1. 概述
`MaterialComponent` 是一个 ECS 组件，用于管理实体的材质实例（MaterialInstance）。它充当 `MaterialInstance` 的容器和代理，允许实体拥有独立的材质参数（如纹理、颜色、统一变量等），或者与其他实体共享同一个材质实例。

## 2. 职责
1.  **持有材质实例**：管理 `MaterialInstance` 的生命周期（通过 `std::shared_ptr`）。
2.  **参数接口**：提供设置纹理、缓冲区、采样器和统一变量（Uniform Data）的便捷接口。
3.  **渲染集成**：为 `RenderSystem` 提供用于绑定的 `DescriptorSet`。

## 3. 数据结构

文件路径：`Engine/Graphics/RHI/Components/MaterialComponent.h`

```cpp
namespace primal::graphics {

class Material;
class MaterialInstance;

namespace rhi {
    class RHIDeviceBase;
    struct MaterialComponent {
        // === 数据成员 ===
        std::shared_ptr<MaterialInstance> materialInstance;
        
        // === 构造函数 ===
        MaterialComponent() = default;
        explicit MaterialComponent(std::shared_ptr<MaterialInstance> instance);
        
        // === 生命周期管理 ===
        /**
         * @brief 基于给定材质创建新的材质实例
         * @param device RHI设备
         * @param material 基础材质
         * @return 是否创建成功
         */
        bool Create(RHIDeviceBase* device, Material* material);
        
        /**
         * @brief 销毁材质实例（如果是独占的）
         */
        void Destroy();
        
        // === 参数设置接口 (转发给 MaterialInstance) ===
        
        void SetTexture(uint32_t binding, ResourceHandle texture);
        void SetBuffer(uint32_t binding, ResourceHandle buffer, uint32_t size, uint32_t offset = 0);
        void SetSampler(uint32_t binding, SamplerHandle sampler);
        void SetUniformData(uint32_t offset, const void* data, uint32_t size);
        
        template<typename T>
        void SetUniform(uint32_t offset, const T& value) {
            SetUniformData(offset, &value, sizeof(T));
        }

        // === 渲染接口 ===
        
        /**
         * @brief 更新 GPU 数据 (在渲染前调用)
         * @param device RHI设备
         * @param frameIndex 当前帧索引
         */
        void Update(RHIDeviceBase* device, uint32_t frameIndex);
        
        /**
         * @brief 获取当前帧的描述符集
         */
        DescriptorSetHandle GetDescriptorSet() const;
        
        /**
         * @brief 检查组件是否有效
         */
        bool IsValid() const;
        
        /**
         * @brief 获取关联的材质
         */
        Material* GetMaterial() const;
    };

} // namespace rhi
} // namespace primal::graphics
```

## 4. 实现细节
- `MaterialComponent` 并不直接管理 GPU 资源句柄的创建和销毁（这些由 `MaterialInstance` 处理）。
- `Create` 方法会 `new` 一个 `MaterialInstance` 并调用其 `Initialize`。
- `Set...` 方法会检查 `materialInstance` 是否有效，有效则转发调用。
- `Update` 方法会设置 `MaterialInstance` 的当前帧索引，并调用其 `Update` 方法。

## 5. 与 RenderSystem 集成
- `RenderSystem` 遍历带有 `MaterialComponent` 的实体。
- 对于每个实体，调用 `component.Update(device, currentFrameIndex)`。
- 获取 `component.GetDescriptorSet()` 并绑定到 CommandBuffer。
- 获取 `component.GetMaterial()->GetPipeline(...)` 并绑定管线。

## 6. 测试计划
- 创建 `MaterialComponent`。
- Mock `Material` 和 `RHIDevice` (或使用 Standalone RHI 环境)。
- 测试 `Create` 是否成功创建 Instance。
- 测试参数设置接口是否正确转发。
- 测试 `GetDescriptorSet`。
