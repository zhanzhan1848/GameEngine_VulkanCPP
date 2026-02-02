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

            void SetTexture(uint32_t binding, ResourceHandle texture);
            void SetBuffer(uint32_t binding, ResourceHandle buffer, uint32_t size, uint32_t offset = 0);
            void SetSampler(uint32_t binding, SamplerHandle sampler);
            void SetUniformData(uint32_t offset, const void* data, uint32_t size);

            template<typename T>
            void SetUniform(uint32_t offset, const T& value) {
                SetUniformData(offset, &value, sizeof(T));
            }

            void Update(RHIDeviceBase* device, uint32_t frameIndex);
            
            DescriptorSetHandle GetDescriptorSet() const;
            
            bool IsValid() const;
            
            Material* GetMaterial() const;
        };
    }
}
