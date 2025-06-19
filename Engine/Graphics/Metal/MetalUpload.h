#pragma once

#include "MetalCommonHeaders.h"
#include <thread>

namespace primal::graphics::metal::upload
{
    class metal_upload_context
    {
    public:
        metal_upload_context(u32 aligend_size);
        DISABLE_COPY_AND_MOVE(metal_upload_context);
        ~metal_upload_context() { assert(_frame_index != u32_invalid_id); }

        void end_upload();

        [[nodiscard]] constexpr MTL::CommandBuffer* const command_buffer() const { return _cmd_buffer; }
        [[nodiscard]] constexpr MTL::Buffer* const upload_buffer() const { return _upload_buffer; }
        [[nodiscard]] constexpr void* const cpu_address() const { return _cpu_address; }
    private:
        DEBUG_OP(metal_upload_context() = default);
        MTL::CommandBuffer*	    _cmd_buffer{ nullptr };
        MTL::Buffer*			_upload_buffer{ nullptr };
        void*					_cpu_address{ nullptr };
        u32						_frame_index{ u32_invalid_id };
    };

    bool initialize();
    void shutdown();
}