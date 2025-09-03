#include "MetalPostProcess.h"

#include "MetalCommonHeaders.h"
#include "MetalCore.h"
#include "MetalShader.h"
#include "MetalSurface.h"
#include "MetalResource.h"
#include "MetalGPass.h"
#include "MetalSSAO.h"

namespace primal::graphics::metal::fx
{
    namespace
    {
        MTL::RenderPipelineState* post_process_pipeline{ nullptr };
        MTL::SamplerState*                      sampler{ nullptr };

        bool create_post_process_pipeline()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            MTL::RenderPipelineDescriptor* info{ MTL::RenderPipelineDescriptor::alloc()->init() };

            info->setVertexFunction( shader::get_engine_shader( shader::engine_shader::fullscreen_triangle_vs ).get()  );
            info->setFragmentFunction( shader::get_engine_shader( shader::engine_shader::post_process_ps ).get() );
            info->colorAttachments()->object(0)->setPixelFormat( MTL::PixelFormat::PixelFormatRGBA16Float );
            post_process_pipeline = device->newRenderPipelineState( info, &pError );
            MTL_CHECK_ERROR(pError)

            return post_process_pipeline != nullptr;
        }

        bool create_sampler()
        {
            MTL::Device* device{ core::get_device() };
            MTL::SamplerDescriptor* sampler_desc{ MTL::SamplerDescriptor::alloc()->init() };
            sampler_desc->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
            sampler_desc->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
            sampler_desc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
            sampler_desc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
            sampler = device->newSamplerState(sampler_desc);
            sampler_desc->release();
            return sampler!= nullptr;
        }
    } // anonymous namespace


    bool initialize()
    {
        return create_post_process_pipeline() && create_sampler();
    }

    void shutdown()
    {
        if (post_process_pipeline) post_process_pipeline->release();
        if (sampler) sampler->release();
    }

    void post_process(MTL::CommandBuffer* buffer, metal_surface* surface, const constant_buffer& cbuffer)
    {
        assert(post_process_pipeline);
        MTK::View* view{ surface->view() };
        CA::MetalDrawable* drawable{ view->currentDrawable() };
        MTL::RenderPassDescriptor* postRpd = view->currentRenderPassDescriptor();
        postRpd->colorAttachments()->object(0)->setClearColor(MTL::ClearColor::Make(0.0f, 0.0f, 0.0f, 1.0f));
        postRpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionClear);
        postRpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);
        postRpd->colorAttachments()->object(0)->setTexture(drawable->texture());
        MTL::RenderCommandEncoder* postEnc = buffer->renderCommandEncoder(postRpd);
        postEnc->setRenderPipelineState( post_process_pipeline );
        postEnc->setFragmentBuffer( cbuffer.buffer(), 0, 0 );
        postEnc->setFragmentSamplerState( sampler, 0 );
        postEnc->setFragmentTexture( gpass::get_depth_buffer().texture(), 0 );
        postEnc->setFragmentTexture( gpass::get_main_buffer().texture(), 1 );
        postEnc->setFragmentTexture( ssao::get_ssao_blur_texture().texture(), 2 );
        postEnc->drawPrimitives( MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6) );
        postEnc->endEncoding();
    }
}