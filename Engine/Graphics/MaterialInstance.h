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
        MaterialInstance(MaterialInstance&& other) noexcept;
        MaterialInstance& operator=(MaterialInstance&& other) noexcept;

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
         * @brief Set buffer parameter
         * @param binding Binding index in the shader
         * @param buffer Buffer resource handle
         * @param size Size of the buffer range to bind
         * @param offset Offset into the buffer
         */
        void SetBuffer(u32 binding, rhi::ResourceHandle buffer, u32 size, u32 offset = 0);

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
         * @brief Set the current frame index for multi-buffering
         * @param frameIndex Current frame index (0 to MAX_FRAMES_IN_FLIGHT-1)
         */
        void SetCurrentFrame(u32 frameIndex);

        /**
         * @brief Update descriptor set on GPU
         * @details Applies pending texture/sampler updates. Uniform data is updated immediately if mapped.
         */
        void Update(rhi::RHIDeviceBase* device);

        /**
         * @brief Get the descriptor set handle for the current frame
         */
        rhi::DescriptorSetHandle GetDescriptorSet() const { 
            if (currentFrameIndex_ < descriptorSets_.size()) {
                return descriptorSets_[currentFrameIndex_];
            }
            return rhi::handles::INVALID_RESOURCE;
        }

        /**
         * @brief Get the parent material
         */
        Material* GetMaterial() const { return material_; }

    private:
        Material* material_{nullptr};
        utl::vector<rhi::DescriptorSetHandle> descriptorSets_;
        
        // Triple buffering for uniforms to avoid CPU-GPU sync stalls
        utl::vector<rhi::ResourceHandle> uniformBuffers_;
        utl::vector<void*> uniformBuffersMapped_;
        u32 currentFrameIndex_{0};
        bool uniformDirty_{false};

        struct TextureUpdate {
            u32 binding;
            rhi::ResourceHandle texture;
        };
        struct SamplerUpdate {
            u32 binding;
            rhi::SamplerHandle sampler;
        };
        struct BufferUpdate {
            u32 binding;
            rhi::ResourceHandle buffer;
            u32 offset;
            u32 size;
        };
        
        utl::vector<TextureUpdate> pendingTextures_;
        utl::vector<SamplerUpdate> pendingSamplers_;
        utl::vector<BufferUpdate> pendingBuffers_;
        
        rhi::RHIDeviceBase* device_{nullptr}; // Cached for destruction
    };

}
