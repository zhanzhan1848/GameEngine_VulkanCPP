#pragma once

#include "ToolsCommon.h"

namespace primal::tools
{
	struct scene;
	struct scene_data;
	struct mesh;
	struct geeometry_import_settings;

	class obj_context
	{
	public:
		


		constexpr f32 scene_scale() const { return _scene_scale; }

	private:

		void load_obj_file(const char* file);
		void get_meshes(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
		void get_mesh(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
		bool get_mesh_data(mesh& m);

		scene*				_scene{ nullptr };
		scene_data*			_scene_data{ nullptr };
		f32					_scene_scale{ 1.f };
	};
}