#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_frame_info;
    class metal_surface;
    class constant_buffer;
}

namespace primal::graphics::metal::fx
{
    bool initialize();
    void shutdown();

    [[nodiscard]] const metal_render_texture& get_compose_texture();

    void set_size(math::u32v2 size);
    void post_process(MTL::CommandBuffer* buffer, metal_surface* surface, const constant_buffer& cbuffer);
    void compose_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}

namespace primal::graphics::metal::ssao
{
    constexpr u32 ssao_tile_size{ 32 };
    constexpr MTL::PixelFormat ssao_texture_format{ MTL::PixelFormatRGBA16Float };
    
    bool initialize();
    void shutdown();

    [[nodiscard]] const metal_render_texture& get_ssao_texture();
    [[nodiscard]] const metal_render_texture& get_ssao_blur_texture();

    void set_size(math::u32v2 size);
    void ssao_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
    void ssao_blur(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}

namespace primal::graphics::metal::ssgi
{
    constexpr u32 ssgi_tile_size{ 32 };
    constexpr MTL::PixelFormat ssgi_texture_format{ MTL::PixelFormatRGBA16Float };
    
    bool initialize();
    void shutdown();

    [[nodiscard]] const metal_render_texture& get_ssgi_texture();
    [[nodiscard]] const metal_render_texture& get_ssgi_blur_texture();

    void set_size(math::u32v2 size);
    void ssgi_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
    void ssgi_blur(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}

namespace primal::graphics::metal::taa
{
    bool initialize();
    void shutdown();

    [[nodiscard]] const metal_render_texture& get_taa_texture();

    void set_size(math::u32v2 size);
    void taa_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}