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

    void post_process(MTL::CommandBuffer* buffer, metal_surface* surface, const constant_buffer& cbuffer);
    void blit_process(MTL::CommandBuffer* buffer, metal_surface* surface, const constant_buffer& cbuffer);
}