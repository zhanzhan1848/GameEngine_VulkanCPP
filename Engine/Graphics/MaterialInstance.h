#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/Material.h"
#include <vector>
#include <mutex>

namespace primal::graphics {

    class MaterialInstance {
    public:
        explicit MaterialInstance(Material* material);
        ~MaterialInstance();

        DISABLE_COPY(MaterialInstance);
        MaterialInstance(MaterialInstance&&) = default;
        MaterialInstance& operator=(MaterialInstance&&) = default;

        /**
         * @brief Initialize the material instance
         * @details Creates descriptor set and uniform buffer based on material configuration
         */
        bool Initialize(rhi::RHIDeviceBase* device);

        /**
         * @brief Set texture parameter
         * @param binding Binding index in the shader
         * @param texture Texture resource handle
         */
        void SetTexture(u32 binding, rhi::ResourceHandle texture);

        /**
         * @brief Set sampler parameter
         * @param binding Binding index in the shader
         * @param sampler Sampler handle
         */
        void SetSampler(u32 binding, rhi::SamplerHandle sampler);

        /**
         * @brief Set uniform data
         * @param offset Offset in the uniform block
         * @param data Pointer to data
         * @param size Size of data
         */
        void SetUniformData(u32 offset, const void* data, u32 size);

        /**
         * @brief Update descriptor set on GPU
         * @details Applies pending texture/sampler updates. Uniform data is updated immediately if mapped.
         */
        void Update(rhi::RHIDeviceBase* device);

        /**
         * @brief Get the descriptor set handle
         */
        rhi::DescriptorSetHandle GetDescriptorSet() const { return descriptorSet_; }

        /**
         * @brief Get the parent material
         */
        Material* GetMaterial() const { return material_; }

    private:
        Material* material_{nullptr};
        rhi::DescriptorSetHandle descriptorSet_{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle uniformBuffer_{rhi::handles::INVALID_RESOURCE};
        void* uniformBufferMapped_{nullptr};
        
        struct TextureUpdate {
            u32 binding;
            rhi::ResourceHandle texture;
        };
        struct SamplerUpdate {
            u32 binding;
            rhi::SamplerHandle sampler;
        };
        
        std::vector<TextureUpdate> pendingTextures_;
        std::vector<SamplerUpdate> pendingSamplers_;
        
        rhi::RHIDeviceBase* device_{nullptr}; // Cached for destruction
    };

}
