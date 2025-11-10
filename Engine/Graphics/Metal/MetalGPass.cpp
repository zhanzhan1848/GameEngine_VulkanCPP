#include "MetalGPass.h"
#include "MetalCore.h"
#include "MetalShader.h"
#include "MetalCamera.h"
#include "MetalContent.h"
#include "MetalLight.h"
#include "shaders/ShaderType.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "MetalPreProcess.h"

namespace primal::graphics::metal::gpass
{
    namespace
    {
        const math::u32v2					initial_dimensions{ 100, 100 };

        metal_render_texture                gpass_world_pos_buffer{};
		metal_render_texture				gpass_normal_depth_buffer{};
		metal_render_texture				gpass_albedo_buffer{};
		metal_render_texture				gpass_motion_vector_buffer{};
        metal_texture                       gpass_depth_buffer{};
        math::u32v2							dimensions{ initial_dimensions };
		utl::vector<MTL::Buffer*>			argument_buffers;

		utl::vector<MTL::Buffer*>			sampler_argument_buffers;
		MTL::SamplerState*                  point_sampler{ nullptr };
		MTL::SamplerState*                  linear_sampler{ nullptr };
		MTL::SamplerState*                  anisotropic_sampler{ nullptr };

#if _DEBUG
		constexpr f32						clear_value[4]{ 0.5f, 0.5f, 0.5f, 1.f };
#else
		constexpr f32						clear_value[4]{ 0.f, 0.f, 0.f, 1.f};
#endif // _DEBUG

		//NOTE (to myself): don't forget to #undef CONSTEXPR when you copy/paste this block of code!
#if USE_STL_VECTOR
#define CONSTEXPR
#else
#define CONSTEXPR constexpr
#endif

        struct gpass_cache
        {
            utl::vector<id::id_type>		metal_render_item_ids;
			u32								descriptor_index_count{ 0 };

			// NOTE: when adding new arrays, make sure to update resize() and struct_size.
 			id::id_type*					entity_ids{ nullptr };
			id::id_type*					submesh_gpass_ids{ nullptr };
			id::id_type*					material_ids{ nullptr };
            MTL::RenderPipelineState**      gpass_pipeline_states{ nullptr };
            MTL::RenderPipelineState**      depth_pipeline_states{ nullptr };
            NS::Array**                   	root_signature{ nullptr };
            material_type::type*			material_types{ nullptr };
			u32**							descriptor_indices{ nullptr };
			u32*							texture_counts{ nullptr };
            MTL::Buffer**					view_buffer{ nullptr };
            buffer_view*					position_buffer_view{ nullptr };
            buffer_view*					element_buffer_view{ nullptr };
            buffer_view*					index_buffer_view{ nullptr };
            MTL::PrimitiveType*             primitive_topologies{ nullptr };
            u32*							elements_types{ nullptr };
            u64*							per_object_data{ nullptr };
			u64*							srv_indices{ nullptr };

            constexpr content::render_item::items_cache items_cache() const
			{
				return {
					entity_ids,
					submesh_gpass_ids,
					material_ids,
					gpass_pipeline_states,
					depth_pipeline_states
				};
			}

            constexpr content::submesh::views_cache views_cache() const
			{
				return {
                    view_buffer,
					position_buffer_view,
					element_buffer_view,
					index_buffer_view,
					primitive_topologies,
					elements_types
				};
			}

			constexpr content::material::materials_cache materials_cache() const
			{
				return {
					root_signature,
					material_types,
					descriptor_indices,
					texture_counts
				};
			}

            CONSTEXPR u32 size() const
			{
				return (u32)metal_render_item_ids.size();
			}

			CONSTEXPR void clear()
			{
				metal_render_item_ids.clear();
				descriptor_index_count = 0;
			}

            CONSTEXPR void resize()
			{
				const u64 items_count{ metal_render_item_ids.size() };
				const u64 new_buffer_size{ items_count * struct_size };
				const u64 old_buffer_size{ _buffer.size() };

				if (new_buffer_size > old_buffer_size)
				{
					_buffer.resize(new_buffer_size);
				}

				if (new_buffer_size != old_buffer_size)
				{
					entity_ids = (id::id_type*)_buffer.data();
					submesh_gpass_ids = (id::id_type*)(&entity_ids[items_count]);
					material_ids = (id::id_type*)(&submesh_gpass_ids[items_count]);
					gpass_pipeline_states = (MTL::RenderPipelineState**)(&material_ids[items_count]);
					depth_pipeline_states = (MTL::RenderPipelineState**)(&gpass_pipeline_states[items_count]);
					root_signature = (NS::Array**)(&depth_pipeline_states[items_count]);
					material_types = (material_type::type*)(&root_signature[items_count]);
					descriptor_indices = (u32**)&material_types[items_count];
					texture_counts = (u32*)&descriptor_indices[items_count];
                    view_buffer = (MTL::Buffer**)(&texture_counts[items_count]);
					position_buffer_view = (buffer_view*)(&view_buffer[items_count]);
					element_buffer_view = (buffer_view*)(&position_buffer_view[items_count]);
					index_buffer_view = (buffer_view*)(&element_buffer_view[items_count]);
					primitive_topologies = (MTL::PrimitiveType*)(&index_buffer_view[items_count]);
					elements_types = (u32*)(&primitive_topologies[items_count]);
					per_object_data = (u64*)(&elements_types[items_count]);
					srv_indices = (u64*)&per_object_data[items_count];
				}
			}
            private:
			constexpr static u32 struct_size{
				sizeof(id::id_type) +							// entity_ids
				sizeof(id::id_type) +							// submesh_gpass_ids
				sizeof(id::id_type) +							// material_ids
				sizeof(MTL::RenderPipelineState*) +				// gpass_pipeline_states
				sizeof(MTL::RenderPipelineState*) +				// depth_pipeline_states
				sizeof(NS::Array*) +								// root_signature
				sizeof(material_type::type) +					// material_types
				sizeof(u32) +									// descriptor_indices
				sizeof(u32) +									// texture_counts
				sizeof(MTL::Buffer*) +				            // view_buffer
				sizeof(buffer_view) +				            // position_buffer_view
				sizeof(buffer_view) +				            // element_buffer_view
				sizeof(buffer_view) +				            // index_buffer_view
				sizeof(MTL::PrimitiveType) +				    // primitive_topologies
				sizeof(u32) +									// elements_types
				sizeof(u64) +									// per_object_data
				sizeof(u64)										// srv_indices
			};

			utl::vector<u8>					_buffer;
        } frame_cache;
#undef CONSTEXPR

        bool create_buffers(math::u32v2 size)
        {
            assert(size.x != 0 && size.y != 0);
            gpass_world_pos_buffer.release();
			gpass_depth_buffer.release();
			gpass_normal_depth_buffer.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x);
            desc->setHeight(size.y);
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            desc->setPixelFormat(main_buffer_format);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

#pragma region gpass_color_texture
            {
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
                gpass_world_pos_buffer = metal_render_texture{ init_info };
            }
#pragma endregion

#pragma region gpass_normal_depth_texture
            {
				metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
                gpass_normal_depth_buffer = metal_render_texture{ init_info };
            }
#pragma endregion

#pragma region gpass_albedo_texture
            {
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
                gpass_albedo_buffer = metal_render_texture{ init_info };
            }
#pragma endregion

#pragma region gpass_motion_vector_texture
            {
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
                gpass_motion_vector_buffer = metal_render_texture{ init_info };
            }
#pragma endregion

#pragma region gpass_depth_texture
            desc->setPixelFormat(depth_buffer_format);
            {
				desc->setTextureType(MTL::TextureType2D);
				desc->setWidth(size.x);
            	desc->setHeight(size.y);
				desc->setArrayLength(1);
				desc->setPixelFormat(depth_buffer_format);
				desc->setStorageMode(MTL::StorageModeShared);
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(0.f, 0.f, 0.f, 0.f);
                gpass_depth_buffer = metal_texture{ init_info };
            }
#pragma endregion

            NAME_METAL_OBJECT(gpass_world_pos_buffer.texture(), "gpass_world_pos_buffer");
            NAME_METAL_OBJECT(gpass_depth_buffer.texture(), "gpass_depth_buffer");
            NAME_METAL_OBJECT(gpass_normal_depth_buffer.texture(), "gpass_normal_depth_buffer");
            NAME_METAL_OBJECT(gpass_albedo_buffer.texture(), "gpass_albedo_buffer");
            NAME_METAL_OBJECT(gpass_motion_vector_buffer.texture(), "gpass_motion_vector_buffer");

			gpass_world_pos_buffer.texture()->setLabel(NS::String::string("gpass_world_pos_buffer", NS::UTF8StringEncoding));
            gpass_depth_buffer.texture()->setLabel(NS::String::string("gpass_depth_buffer", NS::UTF8StringEncoding));
            gpass_normal_depth_buffer.texture()->setLabel(NS::String::string("gpass_normal_depth_buffer", NS::UTF8StringEncoding));
			gpass_albedo_buffer.texture()->setLabel(NS::String::string("gpass_albedo_buffer", NS::UTF8StringEncoding));
			gpass_motion_vector_buffer.texture()->setLabel(NS::String::string("gpass_motion_vector_buffer", NS::UTF8StringEncoding));

            return gpass_world_pos_buffer.texture() != nullptr && gpass_depth_buffer.texture()!= nullptr
					&& gpass_normal_depth_buffer.texture() != nullptr && gpass_albedo_buffer.texture() != nullptr
					&& gpass_motion_vector_buffer.texture() != nullptr;
        }

        void set_root_parameters(u32 cache_index, MTL::Buffer* argument_buffer)
        {
            gpass_cache& cache{ frame_cache };
			assert(cache_index < cache.size());

			MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(cache.root_signature[cache_index]) };
			encoder->setArgumentBuffer(argument_buffer, 0, 0);

            const material_type::type mtl_type{ cache.material_types[cache_index] };
            switch (mtl_type)
			{
			case material_type::opauqe:
			{
				using params = opaque_root_parameter;
				encoder->setBuffer(cache.view_buffer[cache_index], cache.position_buffer_view[cache_index].offset, params::position_buffer);
				encoder->setBuffer(cache.view_buffer[cache_index], cache.element_buffer_view[cache_index].offset, params::element_buffer);
			}
			break;
			default:
			break;
			}
			
			// 释放encoder，防止内存泄漏
			encoder->release();
        }

		void create_sampler(u32 index)
        {
			MTL::Device* device{ core::get_device() };
            MTL::SamplerDescriptor* sampler_desc{ MTL::SamplerDescriptor::alloc()->init() };
			switch (index)
			{
			case 0:
			{
				sampler_desc->setMinFilter(MTL::SamplerMinMagFilterNearest);  // 最近点采样
				sampler_desc->setMagFilter(MTL::SamplerMinMagFilterNearest);
				sampler_desc->setMipFilter(MTL::SamplerMipFilterNearest);
				sampler_desc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setRAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setSupportArgumentBuffers(true);
				point_sampler = device->newSamplerState(sampler_desc);
			} break;
			case 1:
			{
				sampler_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);  // 线性采样
				sampler_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
				sampler_desc->setMipFilter(MTL::SamplerMipFilterLinear);
				sampler_desc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setRAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setSupportArgumentBuffers(true);
				linear_sampler = device->newSamplerState(sampler_desc);
			} break;
			case 2:
			{
				sampler_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);     // 仍然是线性采样
				sampler_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
				sampler_desc->setMipFilter(MTL::SamplerMipFilterLinear);
				sampler_desc->setMaxAnisotropy(8);                              // 设置各向异性等级（通常为 4 或 8）
				sampler_desc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setRAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
				sampler_desc->setSupportArgumentBuffers(true);
				anisotropic_sampler = device->newSamplerState(sampler_desc);
			} break;
			default:
				break;
			}
            
            sampler_desc->release();
        }

		void set_sampler_states(u32 index)
		{
			if(point_sampler == nullptr)
			{
				create_sampler(0);
			}

			if(linear_sampler == nullptr)
			{
				create_sampler(1);
			}

			if(anisotropic_sampler == nullptr)
			{
				create_sampler(2);
			}

			MTL::ArgumentDescriptor* point_sampler_desc{ MTL::ArgumentDescriptor::alloc()->init() };
			point_sampler_desc->setIndex(0);
			point_sampler_desc->setDataType(MTL::DataTypeSampler);
			point_sampler_desc->setAccess(MTL::ArgumentAccessReadOnly);

			MTL::ArgumentDescriptor* linear_sampler_desc{ MTL::ArgumentDescriptor::alloc()->init() };
			linear_sampler_desc->setIndex(1);
			linear_sampler_desc->setDataType(MTL::DataTypeSampler);
			linear_sampler_desc->setAccess(MTL::ArgumentAccessReadOnly);

			MTL::ArgumentDescriptor* anisotropic_sampler_desc{ MTL::ArgumentDescriptor::alloc()->init() };
			anisotropic_sampler_desc->setIndex(2);
			anisotropic_sampler_desc->setDataType(MTL::DataTypeSampler);
			anisotropic_sampler_desc->setAccess(MTL::ArgumentAccessReadOnly);
			
			NS::Object* descs[3]{ point_sampler_desc, linear_sampler_desc, anisotropic_sampler_desc };
			NS::Array* argArray = NS::Array::array(descs, 3);

			MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(argArray) };
			if (index >= sampler_argument_buffers.size())
			{
				sampler_argument_buffers.resize(index + 1, nullptr);
			}
			if(!sampler_argument_buffers[index])
			{
				NS::UInteger argument_buffer_size{ encoder->encodedLength() };
				sampler_argument_buffers[index] = core::get_device()->newBuffer(argument_buffer_size, MTL::ResourceStorageModeShared);
				NAME_METAL_OBJECT(sampler_argument_buffers[index], "gpass_sampler_argument_buffer");
				sampler_argument_buffers[index]->setLabel(NS::String::string("gpass_sampler_argument_buffer", NS::UTF8StringEncoding));
			}
			encoder->setArgumentBuffer(sampler_argument_buffers[index], 0, 0);
			encoder->setSamplerState(point_sampler, 0);
			encoder->setSamplerState(linear_sampler, 1);
			encoder->setSamplerState(anisotropic_sampler, 2);
			
			// 释放encoder，防止内存泄漏
			encoder->release();
		}

		void fill_per_object_data(const metal_frame_info& metal_info)
        {
			constant_buffer& cbuffer{ core::cbuffer() };
            const gpass_cache& cache{ frame_cache };
			const u32 render_items_count{ (u32)cache.size() };
			id::id_type current_entity_id{ id::invalid_id };
            msl::PerObjectData* current_data_pointer{ nullptr };

            for (u32 i{ 0 }; i < render_items_count; ++i)
			{
				if (current_entity_id != cache.entity_ids[i])
				{
					current_entity_id = cache.entity_ids[i];
					msl::PerObjectData data{};
					transform::get_transform_matrices(game_entity::entity_id{ current_entity_id }, data.World, data.InvWorld);
					data.WorldViewProjection = metal_info.camera->view_projection() * data.World;

					current_data_pointer = cbuffer.allocate<msl::PerObjectData>();
					memcpy(current_data_pointer, &data, sizeof(msl::PerObjectData));
				}

				assert(current_data_pointer);
				cache.per_object_data[i] = cbuffer.offset(current_data_pointer);
			}
			
        }

        void prepare_render_frame(const metal_frame_info& metal_info)
        {
            assert(metal_info.info && metal_info.camera);
            assert(metal_info.info->render_item_ids && metal_info.info->render_item_count);
            gpass_cache& cache{ frame_cache };
			cache.clear();

            using namespace content;
            render_item::get_metal_render_item_ids(*metal_info.info, cache.metal_render_item_ids);
            cache.resize();
            const u32 items_count{ cache.size() };
            const render_item::items_cache items_cache{ cache.items_cache() };
            render_item::get_items(cache.metal_render_item_ids.data(), items_count, items_cache);

            const submesh::views_cache views_cache{ cache.views_cache() };
			submesh::get_views(items_cache.submesh_gpu_ids, items_count, views_cache);

            const material::materials_cache materials_cache{ cache.materials_cache() };
			material::get_materials(items_cache.material_ids, items_count, materials_cache, cache.descriptor_index_count);

			fill_per_object_data(metal_info);

			if (cache.descriptor_index_count)
			{
				constant_buffer& cbuffer{ core::cbuffer() };
				const u32 size{ cache.descriptor_index_count * static_cast<u32>(sizeof(u32)) };
				u32 *const srv_indices{ (u32 *const)cbuffer.allocate(size) };
				u32 srv_index_offset{ 0 };

				for (u32 i{ 0 }; i < items_count; ++i)
				{
					const u32 texture_count{ cache.texture_counts[i] };
					cache.srv_indices[i] = 0;

					if (texture_count)
					{
						const u32 *const descriptor_indices{ cache.descriptor_indices[i] };
						memcpy(&srv_indices[srv_index_offset], descriptor_indices, texture_count * sizeof(u32));
						cache.srv_indices[i] = cbuffer.offset(srv_indices + srv_index_offset);
						srv_index_offset += texture_count;
					}
				}
			}
        }
    } // anonymous namespace
    
    bool initialize()
	{
		return create_buffers(initial_dimensions);
	}

	void shutdown()
	{
		gpass_world_pos_buffer.release();
		gpass_normal_depth_buffer.release();
		gpass_albedo_buffer.release();
		gpass_depth_buffer.release();
		gpass_motion_vector_buffer.release();
		dimensions = initial_dimensions;

		if(point_sampler)
		{
			point_sampler->release();
		}

		if(linear_sampler)
		{
			linear_sampler->release();
		}

		if(anisotropic_sampler)
		{
			anisotropic_sampler->release();
		}

		for(auto* sbuffer : sampler_argument_buffers)
		{
			if(sbuffer)
			{
				sbuffer->release();
			}
		}
		sampler_argument_buffers.clear();

		for(auto* buffer : argument_buffers)
		{
			if(buffer)
			{
				buffer->release();
			}
		}
		argument_buffers.clear();
	}

    const metal_render_texture& get_world_pos_buffer()
    {
        return gpass_world_pos_buffer;
    }

    const metal_texture& get_depth_buffer()
    {
        return gpass_depth_buffer;
    }

	const metal_render_texture& get_normal_depth_buffer()
	{
		return gpass_normal_depth_buffer;
	}

	const metal_render_texture& get_albedo_buffer()
	{
		return gpass_albedo_buffer;
	}

	const metal_render_texture& get_motion_vector_buffer()
	{
		return gpass_motion_vector_buffer;
	}

    void set_size(math::u32v2 size)
	{
		math::u32v2& d{ dimensions };
		if (size.x > d.x || size.y > d.y)
		{
			d = { std::max(size.x, d.x), std::max(size.y, d.y) };
			create_buffers(d);
		}
	}

    void depth_prepass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        prepare_render_frame(metal_info);

        const gpass_cache& cache{ frame_cache };
		const u32 items_count{ cache.size() };
		const u32 frame_index{ metal_info.frame_index };

        MTL::RenderPassDescriptor* depthRpd = MTL::RenderPassDescriptor::alloc()->init();

		MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = MTL::RenderPassDepthAttachmentDescriptor::alloc()->init();
		depthAttachment->setClearDepth(1.f);
		depthAttachment->setLoadAction(MTL::LoadActionClear);
		depthAttachment->setStoreAction(MTL::StoreActionStore);
		depthAttachment->setTexture(gpass_depth_buffer.texture());
		depthRpd->setDepthAttachment(depthAttachment);
		depthRpd->setDefaultRasterSampleCount(1);
		depthRpd->setRenderTargetWidth(metal_info.surface_width);
		depthRpd->setRenderTargetHeight(metal_info.surface_height);

		MTL::DepthStencilDescriptor* depthStencil = MTL::DepthStencilDescriptor::alloc()->init();
		depthStencil->setDepthCompareFunction(MTL::CompareFunctionLess);
		depthStencil->setDepthWriteEnabled(true);
		MTL::DepthStencilState* depthStencilState = core::get_device()->newDepthStencilState(depthStencil);

		MTL::RenderCommandEncoder* depthEnc = buffer->renderCommandEncoder(depthRpd);
		depthEnc->setDepthStencilState(depthStencilState);
        
        NS::Array* current_root_signature{ nullptr };
        MTL::RenderPipelineState* current_pipeline_state{ nullptr };

        for (u32 i{ 0 }; i < items_count; ++i)
        {
			// 确保argument_buffers有足够的空间
			if (i >= argument_buffers.size())
			{
				argument_buffers.resize(i + 1, nullptr);
			}

            // 为每个渲染项创建独立的argument buffer，无论root_signature是否相同
			current_root_signature = static_cast<NS::Array*>(cache.root_signature[i]);
			MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(current_root_signature) };
			NS::UInteger argument_buffer_size{ encoder->encodedLength() };

			// 为当前渲染项创建独立的argument buffer
			if(!argument_buffers[i])
			{
				argument_buffers[i] = core::get_device()->newBuffer(argument_buffer_size, MTL::ResourceStorageModeShared);
				NAME_METAL_OBJECT(argument_buffers[i], "depth_argument_buffer");
				argument_buffers[i]->setLabel(NS::String::string("depth_argument_buffer", NS::UTF8StringEncoding));
			}

			encoder->setArgumentBuffer(argument_buffers[i], 0, 0);
			encoder->setBuffer(metal_info.global_shader_data, 0, opaque_root_parameter::global_shader_data);
			encoder->setBuffer(metal_info.global_shader_data, cache.per_object_data[i], opaque_root_parameter::per_object_data);
			metal_info.global_shader_data->setLabel(NS::String::string("global_shader_data", NS::UTF8StringEncoding));
			
			// 立即释放encoder，防止内存泄漏
			encoder->release();

			if (current_pipeline_state != cache.depth_pipeline_states[i])
			{
				current_pipeline_state = cache.depth_pipeline_states[i];
				depthEnc->setRenderPipelineState(current_pipeline_state);
			}

			set_root_parameters(i, argument_buffers[i]);

			depthEnc->setCullMode(MTL::CullMode::CullModeBack);
			depthEnc->setFrontFacingWinding(MTL::Winding::WindingClockwise);
			depthEnc->setVertexBuffer(argument_buffers[i], 0, 0);
			depthEnc->useResource(metal_info.global_shader_data, MTL::ResourceUsageRead);
			depthEnc->useResource(cache.view_buffer[i], MTL::ResourceUsageRead);
			cache.view_buffer[i]->setLabel(NS::String::string("view_buffer", NS::UTF8StringEncoding));

			const buffer_view& view{ cache.index_buffer_view[i] };
			const u32 index_count{ static_cast<u32>(view.size / view.stride) };

			depthEnc->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(index_count), view.stride == sizeof(u16) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, cache.view_buffer[i], view.offset, NS::UInteger(1));
        }

		depthEnc->endEncoding();
		
		// 释放创建的Metal对象，防止内存泄漏
		if (depthStencilState) {
			depthStencilState->release();
		}
		if (depthStencil) {
			depthStencil->release();
		}
		if (depthAttachment) {
			depthAttachment->release();
		}
		if (depthRpd) {
			depthRpd->release();
		}
    }

	void render(MTL::CommandBuffer* buffer, const metal_frame_info& frame_info)
	{
		const gpass_cache& cache{ frame_cache };
		const u32 items_count{ cache.size() };
		const u32 frame_index{ frame_info.frame_index };
		MTL::Buffer* current_non_cullable_light_buffer{ light::non_cullable_light_buffer(frame_index) };
		// MTL::Texture* shadow_mapping_texture{ prepass::prepass_texture().texture() };

		// 创建渲染通道描述符
		MTL::RenderPassDescriptor* gpassRpd = MTL::RenderPassDescriptor::alloc()->init();
		
		// 创建世界坐标附件描述符
		MTL::RenderPassColorAttachmentDescriptor* worldposAttachment = MTL::RenderPassColorAttachmentDescriptor::alloc()->init();
		worldposAttachment->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
		worldposAttachment->setStoreAction(MTL::StoreActionStore);
		worldposAttachment->setLoadAction(MTL::LoadActionClear);
		worldposAttachment->setTexture(gpass_world_pos_buffer.texture());
		gpassRpd->colorAttachments()->setObject(worldposAttachment, 0);

		// Create Normal Depth Texture for SSAO
		MTL::RenderPassColorAttachmentDescriptor* normalAttachment = MTL::RenderPassColorAttachmentDescriptor::alloc()->init();
		normalAttachment->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
		normalAttachment->setStoreAction(MTL::StoreActionStore);
		normalAttachment->setLoadAction(MTL::LoadActionClear);
		normalAttachment->setTexture(gpass_normal_depth_buffer.texture());
		gpassRpd->colorAttachments()->setObject(normalAttachment, 1);

		// Create Albedo Texture
		MTL::RenderPassColorAttachmentDescriptor* albedoAttachment = MTL::RenderPassColorAttachmentDescriptor::alloc()->init();
		albedoAttachment->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
		albedoAttachment->setStoreAction(MTL::StoreActionStore);
		albedoAttachment->setLoadAction(MTL::LoadActionClear);
		albedoAttachment->setTexture(gpass_albedo_buffer.texture());
		gpassRpd->colorAttachments()->setObject(albedoAttachment, 2);

		// Create Motion Vector Texture
		MTL::RenderPassColorAttachmentDescriptor* motionAttachment = MTL::RenderPassColorAttachmentDescriptor::alloc()->init();
		motionAttachment->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
		motionAttachment->setStoreAction(MTL::StoreActionStore);
		motionAttachment->setLoadAction(MTL::LoadActionClear);
		motionAttachment->setTexture(gpass_motion_vector_buffer.texture());
		gpassRpd->colorAttachments()->setObject(motionAttachment, 3);

		// 创建深度附件描述符
		MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = MTL::RenderPassDepthAttachmentDescriptor::alloc()->init();
		depthAttachment->setClearDepth(1.f);
		depthAttachment->setLoadAction(MTL::LoadActionLoad);
		depthAttachment->setStoreAction(MTL::StoreActionStore);
		depthAttachment->setTexture(gpass_depth_buffer.texture());
		gpassRpd->setDepthAttachment(depthAttachment);

		// 创建深度模板描述符
		MTL::DepthStencilDescriptor* depthStencil = MTL::DepthStencilDescriptor::alloc()->init();
		depthStencil->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
		depthStencil->setDepthWriteEnabled(true);
		MTL::DepthStencilState* depthStencilState = core::get_device()->newDepthStencilState(depthStencil);

		gpassRpd->setRenderTargetWidth(frame_info.surface_width);
		gpassRpd->setRenderTargetHeight(frame_info.surface_height);
		gpassRpd->setDefaultRasterSampleCount(1);

		MTL::RenderCommandEncoder* gpassEnc = buffer->renderCommandEncoder(gpassRpd);
		gpassEnc->setDepthStencilState(depthStencilState);
        
        NS::Array* current_root_signature{ nullptr };
        MTL::RenderPipelineState* current_pipeline_state{ nullptr };

		for (u32 i{ 0 }; i < items_count; ++i)
        {
			// 确保argument_buffers有足够的空间
			if (i >= argument_buffers.size())
			{
				argument_buffers.resize(i + 1, nullptr);
			}

            // 为每个渲染项创建独立的argument buffer，无论root_signature是否相同
			current_root_signature = cache.root_signature[i];
			MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(current_root_signature) };
			NS::UInteger argument_buffer_size{ encoder->encodedLength() };

			// 为当前渲染项创建独立的argument buffer
			if(!argument_buffers[i])
			{
				argument_buffers[i] = core::get_device()->newBuffer(argument_buffer_size, MTL::ResourceStorageModeShared);
				NAME_METAL_OBJECT(argument_buffers[i], "gpass_argument_buffer");
				argument_buffers[i]->setLabel(NS::String::string("gpass_argument_buffer", NS::UTF8StringEncoding));
			}

			encoder->setArgumentBuffer(argument_buffers[i], 0, 0);
			encoder->setBuffer(frame_info.global_shader_data, 0, opaque_root_parameter::global_shader_data);
			encoder->setBuffer(frame_info.global_shader_data, cache.per_object_data[i], opaque_root_parameter::per_object_data);
			encoder->setBuffer(current_non_cullable_light_buffer, 0, opaque_root_parameter::directional_lights);
			if(cache.texture_counts[i])
			{
				encoder->setBuffer(frame_info.global_shader_data, cache.srv_indices[i], opaque_root_parameter::srv_indices);
			}
			
			// 立即释放encoder，防止内存泄漏
			encoder->release();
			if (current_pipeline_state != cache.gpass_pipeline_states[i])
			{
				current_pipeline_state = cache.gpass_pipeline_states[i];
				gpassEnc->setRenderPipelineState(current_pipeline_state);
			}

			if(cache.texture_counts[i])
			{
				utl::vector<MTL::Texture*> texture_array{ content::texture::get_texture_array() };
				gpassEnc->setFragmentTextures(texture_array.data(), NS::Range::Make(0, texture_array.size()));
			}

			set_root_parameters(i, argument_buffers[i]);
			set_sampler_states(i);

			gpassEnc->setCullMode(MTL::CullMode::CullModeBack);
			gpassEnc->setFrontFacingWinding(MTL::Winding::WindingClockwise);
			gpassEnc->setVertexBuffer(argument_buffers[i], 0, 0);
			gpassEnc->setFragmentBuffer(argument_buffers[i], 0, 0);
			gpassEnc->setFragmentBuffer(sampler_argument_buffers[i], 0, 1);
			gpassEnc->useResource(frame_info.global_shader_data, MTL::ResourceUsageRead);
			gpassEnc->useResource(cache.view_buffer[i], MTL::ResourceUsageRead);
			gpassEnc->useResource(current_non_cullable_light_buffer, MTL::ResourceUsageRead);

			const buffer_view& view{ cache.index_buffer_view[i] };
			const u32 index_count{ static_cast<u32>(view.size / view.stride) };

			gpassEnc->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(index_count), view.stride == sizeof(u16) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, cache.view_buffer[i], view.offset, NS::UInteger(1));
        }

		gpassEnc->endEncoding();
		
		// 释放创建的Metal对象，防止内存泄漏
		if (depthStencilState) {
			depthStencilState->release();
		}
		if (depthStencil) {
			depthStencil->release();
		}
		if (depthAttachment) {
			depthAttachment->release();
		}
		if (worldposAttachment) {
			worldposAttachment->release();
		}
		if (gpassRpd) {
			gpassRpd->release();
		}
	}
}