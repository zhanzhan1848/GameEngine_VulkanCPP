#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    namespace camera { class metal_camera; }

    struct metal_frame_info
    {
        const frame_info*				info{ nullptr };
        camera::metal_camera*			camera{ nullptr};
        lighting::LightProbeManager*    light_probe_manager{ nullptr };
        MTL::Buffer*					global_shader_data{ nullptr };
		u32								surface_width{ 0 };
		u32								surface_height{ 0 };
		id::id_type						light_culling_id{ id::invalid_id };
		u32								frame_index{ 0 };
		f32								detal_time{ 16.7f };
    };
}

namespace primal::graphics::metal::core
{
    bool initialize();
    void shutdown();

    template<typename T>
    constexpr void release(T*& resource)
    {
        if(resource)
        {
            resource->release();
            resource = nullptr;
        }
    }

    namespace detail
    {
        void deferred_release(MTL::Resource* resource);
    }

    template<typename T>
    constexpr void deferred_release(T*& resource)
    {
        if (resource)
        {
            detail::deferred_release(resource);
            resource = nullptr;
        }
    }

    MTL::Device* get_device();

    // === RHI 桥接（Phase 1 Sub-step 1.2.3'）===
    // 注入外部 MTL::Device（由 rhi::MetalDevice 持有）。设置后 get_device() 优先返回它，
    // create_device() 跳过 MTL::CreateSystemDefaultDevice() 调用。
    // 传 nullptr 清除注入，恢复默认行为。
    // 所有权：set_external_device 会 retain()，shutdown 会 release()。
    // rhi::MetalDevice 自身持有的 mtlDevice_ 不受影响（双引用计数安全）。
    void set_external_device(MTL::Device* device);

    void render(MTK::View* pView);

    [[nodiscard]] u32 current_frame_index();
	void set_deferred_release_flag();
    [[nodiscard]] constant_buffer& cbuffer();

    surface create_surface(platform::window window);
    void remove_surface(surface_id id);
    void resize_surface(surface_id id, u32 width, u32 height);
    u32 surface_width(surface_id id);
    u32 surface_height(surface_id id);
    void render_surface(surface_id id, frame_info info);

    // === Path B (PipelineRenderFrame + BlitRenderTargetToSurface) ===
    // Blit `src_handle` (an rhi::ResourceHandle that resolves to an
    // MTL::Texture* via MetalDevice) into the surface's current drawable
    // and present the drawable. Used by Editor surface rendering:
    // PipelineRenderFrame renders offscreen, BlitRenderTargetToSurface
    // copies into the MTKView drawable. Returns 1 on success, 0 on failure.
    u32 blit_surface_and_present(surface_id id, u64 src_handle);

    // Borrow the MTK::View for a surface (used by MetalBlitToDrawable to
    // grab currentDrawable()). Returns nullptr if id is invalid.
    [[nodiscard]] MTK::View* get_surface_view(surface_id id);
}