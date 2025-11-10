#pragma once
#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_frame_info;
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