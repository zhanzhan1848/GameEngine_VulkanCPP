/**
 * @file MaterialComponent.h
 * @brief 材质组件
 * @details 管理实体的材质实例，提供参数绑定接口
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-30
 * @version 0.1.0
 */

#pragma once

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include <memory>

namespace primal::graphics {
    class Material;
    class MaterialInstance;

    namespace rhi {
        class RHIDeviceBase;
        
        /**
         * @brief 材质组件
         */
        struct MaterialComponent {
            std::shared_ptr<MaterialInstance> materialInstance;

            MaterialComponent() = default;
            explicit MaterialComponent(std::shared_ptr<MaterialInstance> instance);

            /**
             * @brief 基于给定材质创建新的材质实例
             * @param device RHI设备
             * @param material 基础材质
             * @return 是否创建成功
             */
            bool Create(RHIDeviceBase* device, Material* material);

            void SetTexture(u32 binding, ResourceHandle texture);
            void SetBuffer(u32 binding, ResourceHandle buffer, u32 size, u32 offset = 0);
            void SetSampler(u32 binding, SamplerHandle sampler);
            void SetUniformData(u32 offset, const void* data, u32 size);

            template<typename T>
            void SetUniform(u32 offset, const T& value) {
                SetUniformData(offset, &value, sizeof(T));
            }

            void Update(RHIDeviceBase* device, u32 frameIndex);
            
            DescriptorSetHandle GetDescriptorSet() const;
            
            bool IsValid() const;
            
            Material* GetMaterial() const;
        };
    }
}
