#include "MetalPreProcess.h"

#include "MetalCore.h"
#include "MetalLight.h"
#include "MetalShader.h"
#include "MetalCamera.h"
#include "MetalContent.h"
#include "shaders/ShaderType.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "MetalGPass.h"

namespace primal::graphics::metal::prepass
{
    namespace
    {
        constexpr math::u32v2				initial_dimensions{ 2048, 2048 };
        constexpr f32						clear_value[4]{ 1.f, 1.f, 1.f, 1.f};

        metal_render_texture				shadow_mapping_buffer{};
        math::u32v2							dimensions{ initial_dimensions };
        MTL::RenderPipelineState*           shadow_mapping_pipeline{ nullptr };
        utl::vector<MTL::Buffer*>			argument_buffers;

#if USE_STL_VECTOR
#define CONSTEXPR
#else
#define CONSTEXPR constexpr
#endif

        struct prepass_cache
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
				sizeof(NS::Array*) +							// root_signature
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
			shadow_mapping_buffer.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x);
            desc->setHeight(size.y);
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            desc->setPixelFormat(MTL::PixelFormat::PixelFormatDepth32Float);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

#pragma region shadow_mapping_texture
			{
				desc->setTextureType(MTL::TextureType2DArray);
				desc->setArrayLength(3);
				desc->setWidth(2048);
				desc->setHeight(2048);
				desc->setStorageMode(MTL::StorageModeShared);
				metal_texture_init_info init_info{};
				init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
				shadow_mapping_buffer = metal_render_texture{ init_info };
			}
#pragma endregion

			NAME_METAL_OBJECT(shadow_mapping_buffer.texture(), "shadow_mapping_buffer");

			shadow_mapping_buffer.texture()->setLabel(NS::String::string("shadow_mapping_buffer", NS::UTF8StringEncoding));

            desc->release();

            return shadow_mapping_buffer.texture() != nullptr;
        }

        bool create_render_pipeline_state()
        {
            MTL::Device* device{ core::get_device() };
            NS::Error* pError{ nullptr };
            MTL::RenderPipelineDescriptor* info{ MTL::RenderPipelineDescriptor::alloc()->init() };

            // 获取阴影映射顶点着色器并检查有效性
            auto vertex_shader = shader::get_engine_shader( shader::engine_shader::shadow_mapping_vs );
            if (!vertex_shader.get()) {
                info->release();
                return false;
            }

            info->setVertexFunction( vertex_shader.get() );
            info->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
			info->setDepthAttachmentPixelFormat(MTL::PixelFormat::PixelFormatDepth32Float);
            shadow_mapping_pipeline = device->newRenderPipelineState( info, &pError );
            MTL_CHECK_ERROR(pError)

            // 为阴影映射管线状态设置名称
            NAME_METAL_OBJECT(shadow_mapping_pipeline, "shadow_mapping_pipeline");

            info->release();

            return shadow_mapping_pipeline != nullptr;
        }

        void set_root_parameters(u32 cache_index, MTL::Buffer* argument_buffer)
        {
            prepass_cache& cache{ frame_cache };
			assert(cache_index < cache.size());

			MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(cache.root_signature[cache_index]) };
			encoder->setArgumentBuffer(argument_buffer, 0, 0);

            const material_type::type mtl_type{ cache.material_types[cache_index] };
            switch (mtl_type)
			{
			case material_type::opauqe:
			{
				using params = gpass::opaque_root_parameter;
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

		void fill_per_object_data(const metal_frame_info& metal_info)
        {
			constant_buffer& cbuffer{ core::cbuffer() };
            const prepass_cache& cache{ frame_cache };
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
            prepass_cache& cache{ frame_cache };
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
        }
    } // anonymous namespace

    bool initialize()
    {
        return create_buffers(dimensions) && create_render_pipeline_state();
    }

    void shutdown()
    {
        shadow_mapping_buffer.release();
        dimensions = initial_dimensions;

        for(auto* buffer : argument_buffers)
		{
			if(buffer)
			{
				buffer->release();
			}
		}
		argument_buffers.clear();

        if(shadow_mapping_pipeline)
        {
            shadow_mapping_pipeline->release();
            shadow_mapping_pipeline = nullptr;
        }
    }

    const metal_render_texture& prepass_texture()
    {
        return shadow_mapping_buffer;
    }

    void prepass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
		prepare_render_frame(metal_info);
		
        const prepass_cache& cache{ frame_cache };
		const u32 items_count{ cache.size() };
		const u32 frame_index{ metal_info.frame_index };
		MTL::Buffer* current_non_cullable_light_buffer{ light::non_cullable_light_buffer(frame_index) };

        MTL::RenderPassDescriptor* prepassRpd = MTL::RenderPassDescriptor::alloc()->init();

		// Shadow mapping
		prepassRpd->setRenderTargetArrayLength(NS::UInteger(light::non_cullable_light_count(metal_info.info->light_set_key)));
		MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = prepassRpd->depthAttachment();
		depthAttachment->setTexture(shadow_mapping_buffer.texture());
		depthAttachment->setLoadAction(MTL::LoadActionClear);
		depthAttachment->setStoreAction(MTL::StoreActionDontCare);
		depthAttachment->setClearDepth(1.0);

		MTL::DepthStencilDescriptor* depthStencil = MTL::DepthStencilDescriptor::alloc()->init();
		depthStencil->setDepthCompareFunction(MTL::CompareFunctionLess);
		depthStencil->setDepthWriteEnabled(true);
		MTL::DepthStencilState* depthStencilState = core::get_device()->newDepthStencilState(depthStencil);
		depthStencil->release();

		prepassRpd->setDefaultRasterSampleCount(1);

		MTL::RenderCommandEncoder* prepassEnc = buffer->renderCommandEncoder(prepassRpd);
		prepassEnc->setDepthStencilState(depthStencilState);
        
        NS::Array* current_root_signature{ nullptr };

        for (u32 i{ 0 }; i < items_count; ++i)
        {
            // 为每个渲染项创建独立的argument buffer
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
				NAME_METAL_OBJECT(argument_buffers[i], "shadow_argument_buffer");
				argument_buffers[i]->setLabel(NS::String::string("shadow_argument_buffer", NS::UTF8StringEncoding));
			}

			encoder->setArgumentBuffer(argument_buffers[i], 0, 0);
			encoder->setBuffer(metal_info.global_shader_data, 0, gpass::opaque_root_parameter::global_shader_data);
			encoder->setBuffer(metal_info.global_shader_data, cache.per_object_data[i], gpass::opaque_root_parameter::per_object_data);
			encoder->setBuffer(current_non_cullable_light_buffer, 0, gpass::opaque_root_parameter::directional_lights);
			metal_info.global_shader_data->setLabel(NS::String::string("global_shader_data", NS::UTF8StringEncoding));
			
			// 立即释放encoder，防止内存泄漏
			encoder->release();
            prepassEnc->setRenderPipelineState(shadow_mapping_pipeline);

			set_root_parameters(i, argument_buffers[i]);

			prepassEnc->setCullMode(MTL::CullMode::CullModeBack);
			prepassEnc->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
			prepassEnc->setVertexBuffer(argument_buffers[i], 0, 0);
			prepassEnc->useResource(metal_info.global_shader_data, MTL::ResourceUsageRead);
			prepassEnc->useResource(cache.view_buffer[i], MTL::ResourceUsageRead);
			cache.view_buffer[i]->setLabel(NS::String::string("view_buffer", NS::UTF8StringEncoding));
			prepassEnc->useResource(current_non_cullable_light_buffer, MTL::ResourceUsageRead);

			const buffer_view& view{ cache.index_buffer_view[i] };
			const u32 index_count{ static_cast<u32>(view.size / view.stride) };

			prepassEnc->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(index_count), view.stride == sizeof(u16) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, cache.view_buffer[i], view.offset, NS::UInteger(3));
        }

		prepassEnc->endEncoding();
		
		// 释放创建的Metal对象，防止内存泄漏
		if (prepassRpd) {
			prepassRpd->release();
		}

		if(depthStencilState)
		{
			depthStencilState->release();
		}
    }
}