#include "MetalPostProcess.h"

#include "MetalCore.h"
#include "MetalShader.h"
#include "MetalGPass.h"

namespace primal::graphics::metal::taa
{
    namespace
    {
        constexpr math::u32v2					        initial_dimensions{ 100, 100 };
        constexpr f32						            clear_value[4]{ 0.f, 0.f, 0.f, 0.f };
        constexpr MTL::PixelFormat                      taa_format{ MTL::PixelFormat::PixelFormatRGBA16Float };

        metal_render_texture                            prev_taa_texture{};
        metal_render_texture                            curr_taa_texture{};
        MTL::RenderPipelineState*                       taa_pipeline{ nullptr };
        MTL::SamplerState*                              sampler{ nullptr };
        math::u32v2							            dimensions{ initial_dimensions };

        bool create_taa_texture(math::u32v2 size)
        {
            assert(size.x != 0 && size.y != 0);
			curr_taa_texture.release();
            prev_taa_texture.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x);
            desc->setHeight(size.y);
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            desc->setPixelFormat(taa_format);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

            metal_texture_init_info init_info{};
            init_info.texture_desc = desc;
            init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
            prev_taa_texture = metal_render_texture{ init_info };
            curr_taa_texture = metal_render_texture{ init_info };

            NAME_METAL_OBJECT(prev_taa_texture.texture(), "prev_taa_texture");
            NAME_METAL_OBJECT(curr_taa_texture.texture(), "curr_taa_texture");

			prev_taa_texture.texture()->setLabel(NS::String::string("prev_taa_texture", NS::UTF8StringEncoding));
			curr_taa_texture.texture()->setLabel(NS::String::string("curr_taa_texture", NS::UTF8StringEncoding));

            return curr_taa_texture.texture() != nullptr && prev_taa_texture.texture() != nullptr;
        }

        bool create_pipeline()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            MTL::RenderPipelineDescriptor* info{ MTL::RenderPipelineDescriptor::alloc()->init() };

            // 获取着色器函数并检查有效性
            auto vertex_shader = shader::get_engine_shader( shader::engine_shader::fullscreen_triangle_vs );
            auto fragment_shader = shader::get_engine_shader( shader::engine_shader::taa_pass );
            
            if (!vertex_shader.get() || !fragment_shader.get()) {
                info->release();
                return false;
            }

            info->setVertexFunction( vertex_shader.get() );
            info->setFragmentFunction( fragment_shader.get() );
            info->colorAttachments()->object(0)->setPixelFormat( taa_format );
            taa_pipeline = device->newRenderPipelineState( info, &pError );
            info->release();
            MTL_CHECK_ERROR(pError)

            NAME_METAL_OBJECT(taa_pipeline, "taa_pipeline");

            return taa_pipeline != nullptr;
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
            NAME_METAL_OBJECT(sampler, "taa_sampler");
            sampler_desc->release();
            return sampler!= nullptr;
        }
    } // anonymous namespace

    bool initialize()
    {
        return create_taa_texture(initial_dimensions) && 
                create_pipeline() && 
                create_sampler();
    }

    void shutdown()
    {
        if(prev_taa_texture.texture() != nullptr)
        {
            prev_taa_texture.texture()->release();
        }

        if(curr_taa_texture.texture() != nullptr)
        {
            curr_taa_texture.texture()->release();
        }

        if(taa_pipeline != nullptr)
        {
            taa_pipeline->release();
            taa_pipeline = nullptr;
        }
        
        if(sampler != nullptr)
        {
            sampler->release();
            sampler = nullptr;
        }
    }

    const metal_render_texture& get_taa_texture()
    {
        return curr_taa_texture;
    }

    void set_size(math::u32v2 size)
    {
        math::u32v2& d{ dimensions };
		if (size.x > d.x || size.y > d.y)
		{
			d = { std::max(size.x, d.x), std::max(size.y, d.y) };
			create_taa_texture(d);
		}
    }

    void taa_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        assert(taa_pipeline);
        
        // 交换当前帧和历史帧纹理
        std::swap(curr_taa_texture, prev_taa_texture);
        
        // 创建渲染通道描述符
        MTL::RenderPassDescriptor* taaRpd = MTL::RenderPassDescriptor::alloc()->init();
        taaRpd->colorAttachments()->object(0)->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
        taaRpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionClear);
        taaRpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);
        taaRpd->colorAttachments()->object(0)->setTexture(curr_taa_texture.texture());
        
        // 创建渲染命令编码器
        MTL::RenderCommandEncoder* taaEnc = buffer->renderCommandEncoder(taaRpd);
        taaEnc->setRenderPipelineState( taa_pipeline );
        taaEnc->setFragmentSamplerState( sampler, 0 );
        taaEnc->setFragmentTexture( fx::get_compose_texture().texture(), 0 );
        taaEnc->setFragmentTexture( gpass::get_motion_vector_buffer().texture(), 1 );
        taaEnc->setFragmentTexture( gpass::get_normal_depth_buffer().texture(), 2 );
        taaEnc->setFragmentTexture( prev_taa_texture.texture(), 3 );
        taaEnc->drawPrimitives( MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6) );
        taaEnc->endEncoding();
        
        // 释放资源
        taaRpd->release();
    }
}