#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_frame_info;
}

namespace primal::graphics::metal::prepass
{
    bool initialize();
    void shutdown();

    const metal_render_texture& prepass_texture();
    void prepass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info);
}