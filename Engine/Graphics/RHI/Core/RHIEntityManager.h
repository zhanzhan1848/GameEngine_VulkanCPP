#pragma once

#include "RHITypes.h"
#include "Engine/Graphics/RHI/Components/GPUBufferComponent.h"
#include "Engine/Graphics/RHI/Components/TextureComponent.h"
#include "Engine/Graphics/RHI/Components/MaterialComponent.h"
#include "Engine/Graphics/RHI/Components/RenderLayerComponent.h"
#include "Engine/Graphics/RHI/Components/RHITransformComponent.h"
#include "Engine/Utilities/Vector.h"
#include <queue>
#include <type_traits>

namespace primal::graphics::rhi {

    /**
     * @brief RHI 实体管理器
     * @details 管理 RHI 层的实体及其组件，提供 ECS 风格的接口
     */
    class RHIEntityManager {
    public:
        RHIEntityManager() = default;
        ~RHIEntityManager() = default;

        /**
         * @brief 创建一个新的实体
         * @return 新实体的 ID
         */
        RHIEntityID CreateEntity();

        /**
         * @brief 销毁实体及其所有组件
         * @param entity 实体 ID
         */
        void DestroyEntity(RHIEntityID entity);

        /**
         * @brief 检查实体是否存活
         * @param entity 实体 ID
         * @return 是否存活
         */
        bool IsAlive(RHIEntityID entity) const;

        /**
         * @brief 清空所有实体
         */
        void Clear();

        /**
         * @brief 获取当前最大实体索引（用于遍历）
         * @return 最大索引（不包含）
         */
        u32 GetMaxEntityIndex() const { return static_cast<u32>(generations.size()); }

        // === 组件管理 ===

        template<typename T>
        T& AddComponent(RHIEntityID entity);

        template<typename T>
        void RemoveComponent(RHIEntityID entity);

        template<typename T>
        T* GetComponent(RHIEntityID entity);

        template<typename T>
        bool HasComponent(RHIEntityID entity) const;

    private:
        // 实体管理
		utl::vector<u32> generations;
		utl::vector<bool> alive; // Track active state of each index
        std::queue<u32> freeIndices;
        u32 activeEntityCount{0};

        // 组件存储 (SoA)
		utl::vector<GPUBufferComponent> gpuBuffers;
		utl::vector<bool> hasGPUBuffer;

		utl::vector<TextureComponent> textures;
		utl::vector<bool> hasTexture;

		utl::vector<MaterialComponent> materials;
		utl::vector<bool> hasMaterial;

		utl::vector<RenderLayerComponent> renderLayers;
		utl::vector<bool> hasRenderLayer;

		utl::vector<RHITransformComponent> transforms;
		utl::vector<bool> hasTransform;

        // 辅助方法：确保容量
        void EnsureCapacity(u32 index);
    };

    // === 模板实现 ===

    template<typename T>
    T& RHIEntityManager::AddComponent(RHIEntityID entity) {
        static_assert(std::is_same_v<T, GPUBufferComponent> || 
                      std::is_same_v<T, TextureComponent> || 
                      std::is_same_v<T, MaterialComponent> || 
                      std::is_same_v<T, RenderLayerComponent> ||
                      std::is_same_v<T, RHITransformComponent>, 
                      "Unsupported component type");

        u32 index = entity; // 假设 ID 就是索引，忽略 generation 检查（或者在 IsAlive 中检查）
        EnsureCapacity(index);

        if constexpr (std::is_same_v<T, GPUBufferComponent>) {
            hasGPUBuffer[index] = true;
            return gpuBuffers[index] = T{};
        } else if constexpr (std::is_same_v<T, TextureComponent>) {
            hasTexture[index] = true;
            return textures[index] = T{};
        } else if constexpr (std::is_same_v<T, MaterialComponent>) {
            hasMaterial[index] = true;
            return materials[index] = T{};
        } else if constexpr (std::is_same_v<T, RenderLayerComponent>) {
            hasRenderLayer[index] = true;
            return renderLayers[index] = T{};
        } else if constexpr (std::is_same_v<T, RHITransformComponent>) {
            hasTransform[index] = true;
            return transforms[index] = T{};
        }
    }

    template<typename T>
    void RHIEntityManager::RemoveComponent(RHIEntityID entity) {
        u32 index = entity;
        if (index >= generations.size()) return;

        if constexpr (std::is_same_v<T, GPUBufferComponent>) {
            if (index < hasGPUBuffer.size()) hasGPUBuffer[index] = false;
        } else if constexpr (std::is_same_v<T, TextureComponent>) {
            if (index < hasTexture.size()) hasTexture[index] = false;
        } else if constexpr (std::is_same_v<T, MaterialComponent>) {
            if (index < hasMaterial.size()) {
                hasMaterial[index] = false;
                materials[index] = MaterialComponent{}; // 释放引用
            }
        } else if constexpr (std::is_same_v<T, RenderLayerComponent>) {
            if (index < hasRenderLayer.size()) hasRenderLayer[index] = false;
        } else if constexpr (std::is_same_v<T, RHITransformComponent>) {
            if (index < hasTransform.size()) hasTransform[index] = false;
        }
    }

    template<typename T>
    T* RHIEntityManager::GetComponent(RHIEntityID entity) {
        u32 index = entity;
        if (index >= generations.size()) return nullptr;

        if constexpr (std::is_same_v<T, GPUBufferComponent>) {
            if (index < hasGPUBuffer.size() && hasGPUBuffer[index]) return &gpuBuffers[index];
        } else if constexpr (std::is_same_v<T, TextureComponent>) {
            if (index < hasTexture.size() && hasTexture[index]) return &textures[index];
        } else if constexpr (std::is_same_v<T, MaterialComponent>) {
            if (index < hasMaterial.size() && hasMaterial[index]) return &materials[index];
        } else if constexpr (std::is_same_v<T, RenderLayerComponent>) {
            if (index < hasRenderLayer.size() && hasRenderLayer[index]) return &renderLayers[index];
        } else if constexpr (std::is_same_v<T, RHITransformComponent>) {
            if (index < hasTransform.size() && hasTransform[index]) return &transforms[index];
        }
        return nullptr;
    }

    template<typename T>
    bool RHIEntityManager::HasComponent(RHIEntityID entity) const {
        u32 index = entity;
        if (index >= generations.size()) return false;

        if constexpr (std::is_same_v<T, GPUBufferComponent>) {
            return index < hasGPUBuffer.size() && hasGPUBuffer[index];
        } else if constexpr (std::is_same_v<T, TextureComponent>) {
            return index < hasTexture.size() && hasTexture[index];
        } else if constexpr (std::is_same_v<T, MaterialComponent>) {
            return index < hasMaterial.size() && hasMaterial[index];
        } else if constexpr (std::is_same_v<T, RenderLayerComponent>) {
            return index < hasRenderLayer.size() && hasRenderLayer[index];
        } else if constexpr (std::is_same_v<T, RHITransformComponent>) {
            return index < hasTransform.size() && hasTransform[index];
        }
        return false;
    }

} // namespace primal::graphics::rhi
