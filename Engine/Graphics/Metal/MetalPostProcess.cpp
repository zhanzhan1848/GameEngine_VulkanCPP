#include "MetalPostProcess.h"

#include "MetalCommonHeaders.h"
#include "MetalCore.h"
#include "MetalShader.h"
#include "MetalSurface.h"
#include "MetalResource.h"
#include "MetalGPass.h"
#include "MetalLight.h"
#include "MetalPreProcess.h"

namespace primal::graphics::metal::fx
{
    namespace
    {
        const math::u32v2					initial_dimensions{ 100, 100 };
        constexpr f32						clear_value[4]{ 0.f, 0.f, 0.f, 1.f};

        math::u32v2								dimensions{ initial_dimensions };
        MTL::RenderPipelineState*               post_process_pipeline{ nullptr };
        MTL::RenderPipelineState*               compose_pipeline{ nullptr };
        MTL::SamplerState*                      sampler{ nullptr };
        MTL::SamplerState*                      compose_sampler{ nullptr };

        metal_render_texture                    compose_texture{};

        bool create_post_process_pipeline()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            MTL::RenderPipelineDescriptor* info{ MTL::RenderPipelineDescriptor::alloc()->init() };

            // 获取着色器函数并检查有效性
            auto vertex_shader = shader::get_engine_shader( shader::engine_shader::fullscreen_triangle_vs );
            auto fragment_shader = shader::get_engine_shader( shader::engine_shader::post_process_ps );
            
            if (!vertex_shader.get() || !fragment_shader.get()) {
                info->release();
                return false;
            }

            info->setVertexFunction( vertex_shader.get() );
            info->setFragmentFunction( fragment_shader.get() );
            info->colorAttachments()->object(0)->setPixelFormat( MTL::PixelFormat::PixelFormatRGBA16Float );
            post_process_pipeline = device->newRenderPipelineState( info, &pError );
            MTL_CHECK_ERROR(pError)

            NAME_METAL_OBJECT(post_process_pipeline, "post_process_pipeline");

            info->release();

            return post_process_pipeline != nullptr;
        }

        bool create_compose_texture(math::u32v2 size)
        {
            assert(size.x != 0 && size.y != 0);
			compose_texture.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x);
            desc->setHeight(size.y);
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            desc->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

            metal_texture_init_info init_info{};
            init_info.texture_desc = desc;
            init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
            compose_texture = metal_render_texture{ init_info };

            NAME_METAL_OBJECT(compose_texture.texture(), "compose_texture");

			compose_texture.texture()->setLabel(NS::String::string("compose_texture", NS::UTF8StringEncoding));

            desc->release();

            return compose_texture.texture() != nullptr;
        }

        bool create_compose_pipeline()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            MTL::RenderPipelineDescriptor* info{ MTL::RenderPipelineDescriptor::alloc()->init() };

            // 获取着色器函数并检查有效性
            auto vertex_shader = shader::get_engine_shader( shader::engine_shader::fullscreen_triangle_vs );
            auto fragment_shader = shader::get_engine_shader( shader::engine_shader::compose_pass );
            
            if (!vertex_shader.get() || !fragment_shader.get()) {
                info->release();
                return false;
            }

            info->setVertexFunction( vertex_shader.get() );
            info->setFragmentFunction( fragment_shader.get() );
            info->colorAttachments()->object(0)->setPixelFormat( MTL::PixelFormat::PixelFormatRGBA16Float );
            compose_pipeline = device->newRenderPipelineState( info, &pError );
            MTL_CHECK_ERROR(pError)
            NAME_METAL_OBJECT(compose_pipeline, "compose_pipeline");

            info->release();

            return compose_pipeline != nullptr;
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
            compose_sampler = device->newSamplerState(sampler_desc);
            NAME_METAL_OBJECT(sampler, "post_process_sampler");
            NAME_METAL_OBJECT(compose_sampler, "compose_sampler");
            sampler_desc->release();
            return sampler != nullptr && compose_sampler != nullptr;
        }
    } // anonymous namespace

    bool initialize()
    {
        return create_compose_pipeline() && create_sampler() 
                && create_post_process_pipeline() && create_compose_texture(dimensions);
    }

    void shutdown()
    {
        if (post_process_pipeline) 
        {
            post_process_pipeline->release();
            post_process_pipeline = nullptr;
        }
        if (compose_pipeline) 
        {
            compose_pipeline->release();
            compose_pipeline = nullptr;
        }
        if (sampler) 
        {
            sampler->release();
            sampler = nullptr;
        }
        if (compose_sampler) 
        {
            compose_sampler->release();
            compose_sampler = nullptr;
        }
        if (compose_texture.texture()) 
        {
            compose_texture.release();
        }
        dimensions = initial_dimensions;
    }

    const metal_render_texture& get_compose_texture()
    {
        return compose_texture;
    }

    void set_size(math::u32v2 size)
    {
        math::u32v2& d{ dimensions };
		if (size.x > d.x || size.y > d.y)
		{
			d = { std::max(size.x, d.x), std::max(size.y, d.y) };
			create_compose_texture(d);
		}
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
        postEnc->setFragmentTexture( taa::get_taa_texture().texture(), 1 );
        postEnc->setFragmentTexture( ssao::get_ssao_blur_texture().texture(), 2 );
        postEnc->setFragmentTexture( ssgi::get_ssgi_blur_texture().texture(), 3 );
        postEnc->drawPrimitives( MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6) );
        postEnc->endEncoding();
    }

    void compose_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        assert(compose_pipeline);
        auto current_non_cullable_light_buffer{ light::non_cullable_light_buffer(metal_info.frame_index) };

        MTL::RenderPassDescriptor* composeRpd = MTL::RenderPassDescriptor::alloc()->init();
        composeRpd->colorAttachments()->object(0)->setClearColor(MTL::ClearColor::Make(0.0f, 0.0f, 0.0f, 1.0f));
        composeRpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionClear);
        composeRpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);
        composeRpd->colorAttachments()->object(0)->setTexture(compose_texture.texture());
        MTL::RenderCommandEncoder* composeEnc = buffer->renderCommandEncoder(composeRpd);
        composeEnc->setRenderPipelineState( compose_pipeline );
        composeEnc->setFragmentSamplerState( compose_sampler, 0 );
        composeEnc->setFragmentBuffer(metal_info.global_shader_data, 0, 0);
        composeEnc->setFragmentBuffer(current_non_cullable_light_buffer, 0, 1);
        composeEnc->setFragmentTexture( gpass::get_albedo_buffer().texture(), 0 );
        composeEnc->setFragmentTexture( gpass::get_normal_depth_buffer().texture(), 1 );
        composeEnc->setFragmentTexture( gpass::get_world_pos_buffer().texture(), 2);
        composeEnc->setFragmentTexture( prepass::prepass_texture().texture(), 3);
        composeEnc->drawPrimitives( MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6) );
        composeEnc->endEncoding();

        if(composeRpd) composeRpd->release();
    }
}