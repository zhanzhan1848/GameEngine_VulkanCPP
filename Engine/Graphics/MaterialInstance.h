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
         * @param arrayElement Array element index (default 0)
         */
        void SetTexture(u32 binding, rhi::ResourceHandle texture, u32 arrayElement = 0);

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
         * @param arrayElement Array element index (default 0)
         */
        void SetSampler(u32 binding, rhi::SamplerHandle sampler, u32 arrayElement = 0);

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

        // ============================================================================
        // 🔥 NEW METHODS: GPU Material Registry Support
        // ============================================================================

        /**
         * @brief Get texture resource handle at binding index
         * @param binding Texture binding index (0=albedo, 1=normal, 2=ORM)
         * @return Texture resource handle, or INVALID_RESOURCE if not bound
         *
         * @note This retrieves the texture handle from pending textures
         *       For GPU-driven rendering where we need direct resource access
         */
        rhi::ResourceHandle GetTextureHandle(u32 binding) const;

        /**
         * @brief Get material factor values (tint, metallic, roughness)
         * @param out_albedo_tint [out] RGB tint multiplier
         * @param out_metallic [out] Metallic factor (0.0 = dielectric, 1.0 = metal)
         * @param out_roughness [out] Roughness factor (0.0 = smooth, 1.0 = rough)
         *
         * @note Factors are stored in uniform buffer, retrieve from there
         *       Returns default values (1,1,1 tint, 0 metallic, 0.5 roughness) if not set
         */
        void GetMaterialFactors(
            math::v3& out_albedo_tint,
            float& out_metallic,
            float& out_roughness
        ) const;

        /**
         * @brief Get all bound texture handles at once
         * @param out_albedo [out] Albedo texture handle
         * @param out_normal [out] Normal map texture handle
         * @param out_orm [out] ORM (occlusion/roughness/metallic) texture handle
         *
         * @note Convenience method that calls GetTextureHandle for standard bindings
         *       Uses INVALID_RESOURCE for missing textures
         */
        void GetBoundTextures(
            rhi::ResourceHandle& out_albedo,
            rhi::ResourceHandle& out_normal,
            rhi::ResourceHandle& out_orm
        ) const;

        // ============================================================================
        // END NEW METHODS
        // ============================================================================

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
            u32 arrayElement;
            rhi::ResourceHandle texture;
        };
        struct SamplerUpdate {
            u32 binding;
            u32 arrayElement;
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

        // Committed texture state — persists across Update() calls so
        // GPU-driven paths (GPUMaterialRegistry / MaterialDataBuilder) can query
        // bound texture handles at any time, not just before the first Update.
        // Indexed by binding slot; resized on demand in SetTexture.
        utl::vector<rhi::ResourceHandle> boundTextures_;

        rhi::RHIDeviceBase* device_{nullptr}; // Cached for destruction
    };

}
