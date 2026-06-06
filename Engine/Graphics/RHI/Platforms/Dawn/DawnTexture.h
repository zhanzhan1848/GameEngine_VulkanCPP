#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHIResource.h"

namespace primal::graphics::rhi {

class DawnDevice;

/**
 * @brief Dawn WebGPU texture resource implementation
 * @details Wraps WGPUTexture and a default WGPUTextureView with RHIResource
 *          lifecycle management. Supports 1D, 2D, 3D, cube, and array textures
 *          with render target, depth stencil, shader resource, and UAV usage.
 */
class DawnTexture : public RHIResource {
    friend class DawnDevice;

public:
    ~DawnTexture() override;

    /**
     * @brief Get the native WGPUTexture handle
     */
    WGPUTexture GetNativeTexture() const { return wgpuTexture_; }

    /**
     * @brief Get the default texture view (created at initialization)
     */
    WGPUTextureView GetDefaultView() const { return wgpuTextureView_; }

    /**
     * @brief Get the texture descriptor
     */
    const TextureDesc& GetTextureDesc() const { return textureDesc_; }

    /**
     * @brief Create a custom texture view from this texture
     * @param viewDesc  Descriptor specifying mip range, array slices, format, etc.
     * @return A new WGPUTextureView, or nullptr on failure. Caller is responsible
     *         for releasing the returned view via wgpuTextureViewRelease().
     */
    WGPUTextureView CreateView(const TextureViewDesc& viewDesc);

    /**
     * @brief Override native handles with an external WGPUTexture (e.g. swapchain surface).
     *        Used by DawnDevice::WrapSurfaceTexture.
     */
    void OverrideNativeTexture(WGPUTexture texture, WGPUTextureView view);

    /**
     * @brief Clear native handles without releasing them (swapchain owns them).
     */
    void ClearNativeHandles();

    /**
     * @brief Release the texture view we created, but not the surface-owned texture.
     *        Used by DawnDevice::ReleaseSurfaceTexture.
     */
    void ReleaseWrappedHandles();

protected:
    bool Initialize() override;
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override { (void)offset; (void)size; return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* data, u64 size, u64 offset) override { (void)data; (void)size; (void)offset; return false; }

public:
    /**
     * @brief Construct a DawnTexture
     * @param device  Reference to the owning DawnDevice
     * @param desc    Texture descriptor (type, format, dimensions, usage, etc.)
     */
    DawnTexture(DawnDevice& device, const TextureDesc& desc);

private:
    DawnDevice& device_;
    WGPUTexture wgpuTexture_ = nullptr;
    WGPUTextureView wgpuTextureView_ = nullptr;
    TextureDesc textureDesc_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
