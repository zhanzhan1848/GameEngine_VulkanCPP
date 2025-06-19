#include "MetalHelper.h"

#include "MetalCore.h"

namespace primal::graphics::metal
{
    MTL::RenderPipelineState* create_pipeline_status(void* stream)
    {
        assert(stream);
        metal_pipeline_state_stream* pipeline_stream = static_cast<metal_pipeline_state_stream*>(stream);
        MTL::Device* device{ core::get_device() };
        NS::Error* pError{ nullptr };
        MTL::RenderPipelineState* pipeline_state{ device->newRenderPipelineState(pipeline_stream->ToDescriptor(), &pError) };
        MTL_CHECK_ERROR(pError);
        return pipeline_state;
    }
}