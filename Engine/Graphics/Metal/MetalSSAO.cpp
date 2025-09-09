#include "MetalSSAO.h"

#include "MetalCore.h"
#include "MetalResource.h"
#include "MetalCamera.h"
#include "MetalShader.h"
#include "MetalGPass.h"
#include "MetalLight.h"
#include "shaders/ShaderType.h"

namespace primal::graphics::metal::ssao
{
    namespace 
    {
        struct ssao_parameters
		{
			msl::SSAODispatchParameters		            ssao_dispatch_params{};
			u32											view_height{ 0 };
			u32											view_width{ 0 };
			f32											camera_fov{ 0.f };
		};

        metal_render_texture                            ssao_texture{};
        metal_render_texture                            ssao_blur_texture{};

        MTL::ComputePipelineState*                      ssao_pipeline_state{ nullptr };
        MTL::ComputePipelineState*                      ssao_blur_pipeline_state{ nullptr };

        constexpr math::u32v2					        initial_dimensions{ 100, 100 };
        constexpr f32						            clear_value[4]{ 1.f, 1.f, 1.f, 1.f};
        math::u32v2							            dimensions{ initial_dimensions };
        ssao_parameters                                 ssao_params{};

        bool create_ssao_texture(math::u32v2 size)
        {
            assert(size.x != 0 && size.y != 0);
			ssao_texture.release();
            ssao_blur_texture.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x);
            desc->setHeight(size.y);
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
            desc->setPixelFormat(ssao::ssao_texture_format);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

            metal_texture_init_info init_info{};
            init_info.texture_desc = desc;
            init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
            ssao_texture = metal_render_texture{ init_info };
            ssao_blur_texture = metal_render_texture{ init_info };

            NAME_METAL_OBJECT(ssao_texture.texture(), "ssao_texture");
            NAME_METAL_OBJECT(ssao_blur_texture.texture(), "ssao_blur_texture");

			ssao_texture.texture()->setLabel(NS::String::string("ssao_texture", NS::UTF8StringEncoding));
            ssao_blur_texture.texture()->setLabel(NS::String::string("ssao_blur_texture", NS::UTF8StringEncoding));

            return ssao_texture.texture() != nullptr && ssao_blur_texture.texture() != nullptr;
        }

        bool create_pipeline_state()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            ssao_pipeline_state = device->newComputePipelineState(shader::get_engine_shader( shader::engine_shader::ssao_calculate ).get(), &pError);
            MTL_CHECK_ERROR(pError)

            ssao_blur_pipeline_state = device->newComputePipelineState(shader::get_engine_shader(shader::engine_shader::ssao_blur).get(), &pError);
            MTL_CHECK_ERROR(pError);

            return ssao_pipeline_state != nullptr && ssao_blur_pipeline_state != nullptr;
        }

        void resize(ssao_parameters& culler)
		{
			constexpr u32 tile_size{ ssao_tile_size };
			assert(culler.view_width >= tile_size && culler.view_height >= tile_size);
			const math::u32v2 tile_count
			{
				(u32)math::align_size_up<tile_size>(culler.view_width) / tile_size,
				(u32)math::align_size_up<tile_size>(culler.view_height) / tile_size
			};

			// Dispatch parameters for grid frustums
			{
				msl::SSAODispatchParameters& params{ culler.ssao_dispatch_params };
				params.NumThreads = tile_count;
				params.NumThreadGroups.x = (u32)math::align_size_up<tile_size>(tile_count.x) / tile_size;
				params.NumThreadGroups.y = (u32)math::align_size_up<tile_size>(tile_count.y) / tile_size;
			}

			// Dispatch parameters for light culling
			{
				msl::SSAODispatchParameters& params{ culler.ssao_dispatch_params };
				params.NumThreads.x = tile_count.x * tile_size;
				params.NumThreads.y = tile_count.y * tile_size;
				params.NumThreadGroups = tile_count;
			}
		}

        void __attribute__((noinline)) resize(ssao_parameters& culler, const metal_frame_info& metal_info)
		{
			culler.camera_fov = metal_info.camera->field_of_view();
			culler.view_width = metal_info.surface_width;
			culler.view_height = metal_info.surface_height;

			resize(culler);
		}
    } //anonymous namespace

    bool initialize()
    {
        return create_ssao_texture(initial_dimensions) && create_pipeline_state();
    }   

    void shutdown()
    {
        ssao_texture.release();
        ssao_blur_texture.release();

        if(ssao_pipeline_state) ssao_pipeline_state->release();
        if(ssao_blur_pipeline_state) ssao_blur_pipeline_state->release();
    }

    void set_size(math::u32v2 size)
	{
		math::u32v2& d{ dimensions };
		if (size.x > d.x || size.y > d.y)
		{
			d = { std::max(size.x, d.x), std::max(size.y, d.y) };
			create_ssao_texture(d);
		}
	}

    const metal_render_texture& get_ssao_texture()
    {
        return ssao_texture;
    }

    const metal_render_texture& get_ssao_blur_texture()
    {
        return ssao_blur_texture;
    }

    void ssao_pass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        const u32 frame_index{ metal_info.frame_index };
		MTL::Buffer* current_non_cullable_light_buffer{ light::non_cullable_light_buffer(frame_index) };

        if (metal_info.surface_width != ssao_params.view_width &&
			metal_info.surface_height != ssao_params.view_height &&
			!math::is_equal(metal_info.camera->field_of_view(), ssao_params.camera_fov))
		{
			resize(ssao_params, metal_info);
		}

        MTL::ComputeCommandEncoder* encoder{ buffer->computeCommandEncoder() };
        encoder->setComputePipelineState(ssao_pipeline_state);
        encoder->setTexture(gpass::get_normal_depth_buffer().texture(), 0);
        encoder->setTexture(gpass::get_albedo_buffer().texture(), 1);
        encoder->setTexture(ssao_texture.texture(), 2);
        encoder->setBuffer(metal_info.global_shader_data, 0, 0);
        encoder->setBuffer(current_non_cullable_light_buffer, 0, 1);

        encoder->dispatchThreads(MTL::Size::Make(ssao_params.ssao_dispatch_params.NumThreads.x, ssao_params.ssao_dispatch_params.NumThreads.y, 1), MTL::Size::Make(ssao_tile_size, ssao_tile_size, 1));
        encoder->endEncoding();
    }

    void ssao_blur(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        if (metal_info.surface_width != ssao_params.view_width &&
			metal_info.surface_height != ssao_params.view_height &&
			math::is_equal(metal_info.camera->field_of_view(), ssao_params.camera_fov))
		{
			resize(ssao_params, metal_info);
		}

        MTL::ComputeCommandEncoder* encoder{ buffer->computeCommandEncoder() };
        encoder->setComputePipelineState(ssao_blur_pipeline_state);
        encoder->setTexture(ssao_texture.texture(), 0);
        encoder->setTexture(ssao_blur_texture.texture(), 1);
        encoder->setTexture(gpass::get_normal_depth_buffer().texture(), 2);
        encoder->setBuffer(metal_info.global_shader_data, 0, 0);
        encoder->dispatchThreads(MTL::Size::Make(ssao_params.ssao_dispatch_params.NumThreads.x, ssao_params.ssao_dispatch_params.NumThreads.y, 1), MTL::Size::Make(ssao_tile_size, ssao_tile_size, 1));
        encoder->endEncoding();
    }
}