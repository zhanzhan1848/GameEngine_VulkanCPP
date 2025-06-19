#pragma once
#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
	struct buffer_view
	{
		NS::UInteger		offset{ 0 };
		NS::UInteger		size{ 0 };
		NS::UInteger		stride{ 0 };
	};
}

namespace primal::graphics::metal::content
{
	bool initialize();
	void shutdown();

    namespace submesh
	{
		struct views_cache
		{
			MTL::Buffer **const							view_buffer;
			buffer_view *const							position_buffer_view;
			buffer_view *const							element_buffer_view;
			buffer_view *const							index_buffer_view;
			MTL::PrimitiveType  *const					primitive_topologies;
			u32 *const									elements_types;
		};

		id::id_type add(const u8*& data);
		void remove(id::id_type id);
		void get_views(const id::id_type *const gpu_ids, u32 id_count, const views_cache& cache);
	} // submesh namespace

	namespace texture
	{
		id::id_type add(const u8* const);
		void remove(id::id_type);
		void get_descriptor_indices(const id::id_type *const texture_ids, u32 id_count, u32 *const indices);
	} // texture namespace

	namespace material
	{
		struct materials_cache
		{
			NS::Array* *const							argument_buffer_layouts;
			material_type::type *const					material_types;
		};

		id::id_type add(material_init_info info);
		void remove(id::id_type id);
		void get_materials(const id::id_type *const material_ids, u32 material_count, const materials_cache& cache);
	} // namespace material

	namespace render_item
	{

		struct items_cache
		{
			id::id_type *const								entity_ids;
			id::id_type *const								submesh_gpu_ids;
			id::id_type *const								material_ids;
			MTL::RenderPipelineState* *const				gpass_psos;
			MTL::RenderPipelineState* *const				depth_psos;
		};

		id::id_type add(id::id_type entity_id, id::id_type geometry_content_id, u32 material_count, const id::id_type *const material_ids);
		void remove(id::id_type id);
		void get_metal_render_item_ids(const frame_info& info, utl::vector<id::id_type>& metal_render_item_ids);
		void get_items(const id::id_type *const metal_render_item_ids, u32 id_count, const items_cache& cache);
	} // namespace render_item
}