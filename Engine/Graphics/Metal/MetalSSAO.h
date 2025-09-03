#pragma once
#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_frame_info;
}

namespace primal::graphics::metal::ssao
{
    constexpr u32 ssao_tile_szie{ 32 };
    constexpr MTL::PixelFormat ssao_texture_format{ MTL::PixelFormatRGBA16Float };
    
    bool initialize();
    void shutdown();

    [[nodiscard]] const metal_render_texture& get_ssao_texture();
    [[nodiscard]] const metal_render_texture& get_ssao_blur_texture();

    void set_size(math::u32v2 size);
    void ssao_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
    void ssao_blur(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}