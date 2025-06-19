#include "MetalGPass.h"
#include "MetalCore.h"
#include "MetalShader.h"
#include "MetalCamera.h"
#include "MetalContent.h"
#include "shaders/ShaderType.h"
#include "Components/Entity.h"
#include "Components/Transform.h"


namespace primal::graphics::metal::gpass
{
    namespace
    {
        const math::u32v2					initial_dimensions{ 100, 100 };

        metal_render_texture                gpass_main_buffer{};
        metal_texture                       gpass_depth_buffer{};
        math::u32v2							dimensions{ initial_dimensions };
		MTL::Buffer*						argument_buffer{ nullptr };

#if _DEBUG
		constexpr f32						clear_value[4]{ 0.5f, 0.5f, 0.5f, 1.f };
#else
		constexpr f32						clear_value[4]{ };
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

			// NOTE: when adding new arrays, make sure to update resize() and struct_size.
 			id::id_type*					entity_ids{ nullptr };
			id::id_type*					submesh_gpass_ids{ nullptr };
			id::id_type*					material_ids{ nullptr };
            MTL::RenderPipelineState**      gpass_pipeline_states{ nullptr };
            MTL::RenderPipelineState**      depth_pipeline_states{ nullptr };
            NS::Array**                   	root_signature{ nullptr };
            material_type::type*			material_types{ nullptr };
            MTL::Buffer**					view_buffer{ nullptr };
            buffer_view*					position_buffer_view{ nullptr };
            buffer_view*					element_buffer_view{ nullptr };
            buffer_view*					index_buffer_view{ nullptr };
            MTL::PrimitiveType*             primitive_topologies{ nullptr };
            u32*							elements_types{ nullptr };
            u64*							per_object_data{ nullptr };

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
					material_types
				};
			}

            CONSTEXPR u32 size() const
			{
				return (u32)metal_render_item_ids.size();
			}

			CONSTEXPR void clear()
			{
				metal_render_item_ids.clear();
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
                    view_buffer = (MTL::Buffer**)(&material_types[items_count]);
					position_buffer_view = (buffer_view*)(&view_buffer[items_count]);
					element_buffer_view = (buffer_view*)(&position_buffer_view[items_count]);
					index_buffer_view = (buffer_view*)(&element_buffer_view[items_count]);
					primitive_topologies = (MTL::PrimitiveType*)(&index_buffer_view[items_count]);
					elements_types = (u32*)(&primitive_topologies[items_count]);
					per_object_data = (u64*)(&elements_types[items_count]);
				}
			}
            private:
			constexpr static u32 struct_size{
				sizeof(id::id_type) +							// entity_ids
				sizeof(id::id_type) +							// submesh_gpass_ids
				sizeof(id::id_type) +							// material_ids
				sizeof(MTL::RenderPipelineState) +				// gpass_pipeline_states
				sizeof(MTL::RenderPipelineState) +				// depth_pipeline_states
				sizeof(NS::Array) +							// root_signature
				sizeof(material_type::type) +					// material_types
				sizeof(MTL::Buffer) +				            // view_buffer
				sizeof(NS::UInteger) +				            // position_buffers
				sizeof(NS::UInteger) +				            // element_buffers
				sizeof(buffer_view) +				            // index_buffer_views
				sizeof(MTL::PrimitiveType) +				    // primitive_topologies
				sizeof(u32) +									// elements_types
				sizeof(u64)				                // per_object_data
			};

			utl::vector<u8>					_buffer;
        } frame_cache;
#undef CONSTEXPR

        bool create_buffers(math::u32v2 size)
        {
            assert(size.x() != 0 && size.y() != 0);
            gpass_main_buffer.release();
			gpass_depth_buffer.release();

            MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
            desc->setWidth(size.x());
            desc->setHeight(size.y());
            desc->setDepth(1);
            desc->setStorageMode(MTL::StorageModeShared);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            desc->setPixelFormat(main_buffer_format);
            desc->setTextureType(MTL::TextureType2D);
            desc->setMipmapLevelCount(1);
            desc->setArrayLength(1);

            {
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
                gpass_main_buffer = metal_render_texture{ init_info };
            }

            desc->setPixelFormat(depth_buffer_format);
            {
                metal_texture_init_info init_info{};
                init_info.texture_desc = desc;
                init_info.clear_value = MTL::ClearColor::Make(0.f, 0.f, 0.f, 0.f);
                gpass_depth_buffer = metal_texture{ init_info };
            }
            NAME_METAL_OBJECT(gpass_main_buffer.texture(), "gpass_main_buffer");
            NAME_METAL_OBJECT(gpass_depth_buffer.texture(), "gpass_depth_buffer");

			gpass_main_buffer.texture()->setLabel(NS::String::string("gpass_main_buffer", NS::UTF8StringEncoding));
            gpass_depth_buffer.texture()->setLabel(NS::String::string("gpass_depth_buffer", NS::UTF8StringEncoding));

            return gpass_main_buffer.texture() != nullptr && gpass_depth_buffer.texture()!= nullptr;
        }

        void set_root_parameters(u32 cache_index, MTL::ArgumentEncoder* encoder)
        {
            gpass_cache& cache{ frame_cache };
			assert(cache_index < cache.size());

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
			material::get_materials(items_cache.material_ids, items_count, materials_cache);
        }

        void fill_per_object_data(constant_buffer& cbuffer, const metal_frame_info& metal_info)
        {
            const gpass_cache& cache{ frame_cache };
			const u32 render_items_count{ (u32)cache.size() };
			id::id_type current_entity_id{ id::invalid_id };
            msl::PerObjectData* current_data_pointer{ nullptr };

			auto device{ core::get_device() };

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
    } // anonymous namespace
    
    bool initialize()
	{
		return create_buffers(initial_dimensions);
	}

	void shutdown()
	{
		gpass_main_buffer.release();
		gpass_depth_buffer.release();
		dimensions = initial_dimensions;

	}

    const metal_render_texture& get_main_buffer()
    {
        return gpass_main_buffer;
    }

    const metal_texture& get_depth_buffer()
    {
        return gpass_depth_buffer;
    }

    void set_size(math::u32v2 size)
	{
		math::u32v2& d{ dimensions };
		if (size.x() > d.x() || size.y() > d.y())
		{
			d = { std::max(size.x(), d.x()), std::max(size.y(), d.y()) };
			create_buffers(d);
		}
	}

    void depth_prepass(MTL::CommandBuffer* buffer, const metal_frame_info& metal_info)
    {
        prepare_render_frame(metal_info);
		constant_buffer& cbuffer{ core::cbuffer() };
		fill_per_object_data(cbuffer, metal_info);

        const gpass_cache& cache{ frame_cache };
		const u32 items_count{ cache.size() };

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
		depthStencil->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
		depthStencil->setDepthWriteEnabled(true);
		MTL::DepthStencilState* depthStencilState = core::get_device()->newDepthStencilState(depthStencil);

		MTL::RenderCommandEncoder* depthEnc = buffer->renderCommandEncoder(depthRpd);
        
        NS::Array* current_root_signature{ nullptr };
        MTL::RenderPipelineState* current_pipeline_state{ nullptr };

        for (u32 i{ 0 }; i < items_count; ++i)
        {
			
            if (current_root_signature != cache.root_signature[i])
			{
				current_root_signature = static_cast<NS::Array*>(cache.root_signature[i]);
				MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(current_root_signature) };
				NS::UInteger argument_buffer_size{ encoder->encodedLength() };

				// 每次更换root signature时都重新创建和设置argument buffer
				if(!argument_buffer)
				{
					argument_buffer = core::get_device()->newBuffer(argument_buffer_size, MTL::ResourceStorageModeShared);
					NAME_METAL_OBJECT(argument_buffer, "gpass_argument_buffer");
					argument_buffer->setLabel(NS::String::string("gpass_argument_buffer", NS::UTF8StringEncoding));
				}

				encoder->setArgumentBuffer(argument_buffer, 0, 0);
				encoder->setBuffer(metal_info.global_shader_data, 0, opaque_root_parameter::global_shader_data);
				encoder->setBuffer(metal_info.global_shader_data, cache.per_object_data[i], opaque_root_parameter::per_object_data);
				metal_info.global_shader_data->setLabel(NS::String::string("global_shader_data", NS::UTF8StringEncoding));
				set_root_parameters(i, encoder);
			}
			if (current_pipeline_state != cache.depth_pipeline_states[i])
			{
				current_pipeline_state = cache.depth_pipeline_states[i];
				depthEnc->setRenderPipelineState(current_pipeline_state);
				depthEnc->setDepthStencilState(depthStencilState);
			}

			depthEnc->setCullMode(MTL::CullMode::CullModeBack);
			depthEnc->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
			depthEnc->setVertexBuffer(argument_buffer, 0, 0);
			depthEnc->useResource(metal_info.global_shader_data, MTL::ResourceUsageRead);
			depthEnc->useResource(cache.view_buffer[i], MTL::ResourceUsageRead);
			cache.view_buffer[i]->setLabel(NS::String::string("view_buffer", NS::UTF8StringEncoding));

			const buffer_view& view{ cache.index_buffer_view[i] };
			const u32 index_count{ static_cast<u32>(view.size >> view.stride) };

			depthEnc->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(index_count), view.stride == 1 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, cache.view_buffer[i], view.offset, NS::UInteger(1));
        }

		depthEnc->endEncoding();
    }

	void render(MTL::CommandBuffer* buffer, const metal_frame_info& frame_info)
	{
		const gpass_cache& cache{ frame_cache };
		const u32 items_count{ cache.size() };
		const u32 frame_index{ frame_info.frame_index };

		MTL::RenderPassDescriptor* gpassRpd = MTL::RenderPassDescriptor::alloc()->init();
		MTL::RenderPassColorAttachmentDescriptor* colorAttachment = MTL::RenderPassColorAttachmentDescriptor::alloc()->init();
		colorAttachment->setClearColor(MTL::ClearColor::Make(clear_value[0], clear_value[1], clear_value[2], clear_value[3]));
		colorAttachment->setStoreAction(MTL::StoreActionStore);
		colorAttachment->setLoadAction(MTL::LoadActionClear);
		colorAttachment->setTexture(gpass_main_buffer.texture());
		gpassRpd->colorAttachments()->setObject(colorAttachment, 0);

		MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = MTL::RenderPassDepthAttachmentDescriptor::alloc()->init();
		depthAttachment->setClearDepth(1.f);
		depthAttachment->setLoadAction(MTL::LoadActionLoad);
		depthAttachment->setStoreAction(MTL::StoreActionDontCare);
		depthAttachment->setTexture(gpass_depth_buffer.texture());
		gpassRpd->setDepthAttachment(depthAttachment);

		MTL::DepthStencilDescriptor* depthStencil = MTL::DepthStencilDescriptor::alloc()->init();
		depthStencil->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
		depthStencil->setDepthWriteEnabled(false);
		MTL::DepthStencilState* depthStencilState = core::get_device()->newDepthStencilState(depthStencil);

		gpassRpd->setRenderTargetWidth(frame_info.surface_width);
		gpassRpd->setRenderTargetHeight(frame_info.surface_height);
		gpassRpd->setDefaultRasterSampleCount(1);

		MTL::RenderCommandEncoder* gpassEnc = buffer->renderCommandEncoder(gpassRpd);
        
        NS::Array* current_root_signature{ nullptr };
        MTL::RenderPipelineState* current_pipeline_state{ nullptr };

		for (u32 i{ 0 }; i < items_count; ++i)
        {
			
            if (current_root_signature != cache.root_signature[i])
			{
				current_root_signature = cache.root_signature[i];
				MTL::ArgumentEncoder* encoder{ core::get_device()->newArgumentEncoder(current_root_signature) };
				NS::UInteger argument_buffer_size{ encoder->encodedLength() };

				// 每次更换root signature时都重新创建和设置argument buffer
				if(!argument_buffer)
				{
					argument_buffer = core::get_device()->newBuffer(argument_buffer_size, MTL::ResourceStorageModeShared);
					NAME_METAL_OBJECT(argument_buffer, "gpass_argument_buffer");
					
				}

				encoder->setArgumentBuffer(argument_buffer, 0, 0);
				encoder->setBuffer(frame_info.global_shader_data, 0, opaque_root_parameter::global_shader_data);
				encoder->setBuffer(frame_info.global_shader_data, cache.per_object_data[i], opaque_root_parameter::per_object_data);
				set_root_parameters(i,encoder);
			}
			if (current_pipeline_state != cache.gpass_pipeline_states[i])
			{
				current_pipeline_state = cache.gpass_pipeline_states[i];
				gpassEnc->setRenderPipelineState(current_pipeline_state);
			}

			gpassEnc->setDepthStencilState(depthStencilState);
			gpassEnc->setCullMode(MTL::CullMode::CullModeBack);
			gpassEnc->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
			gpassEnc->setVertexBuffer(argument_buffer, 0, 0);
			gpassEnc->useResource(frame_info.global_shader_data, MTL::ResourceUsageRead);
			gpassEnc->useResource(cache.view_buffer[i], MTL::ResourceUsageRead);

			const buffer_view& view{ cache.index_buffer_view[i] };
			const u32 index_count{ static_cast<u32>(view.size >> view.stride) };

			gpassEnc->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(index_count), view.stride == 1 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, cache.view_buffer[i], view.offset, NS::UInteger(1));
        }

		gpassEnc->endEncoding();
	}
}