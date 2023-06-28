#pragma once

#include "ToolsCommon.h"

namespace primal::tools
{
	struct scene;
	struct scene_data;
	struct mesh;
	struct geeometry_import_settings;

	struct mtl_config
	{
		char				name[256];
		char*				shader_name;
		u8					auto_release;
		f32					IOR;
		math::v4			ambient_color;
		math::v4			diffuse_color;
		math::v4			specular_color;
		math::v4			transmission;
		f32					shininess;
		char				diffuse_map_name[256];
		char				specular_map_name[256];
		char				normal_map_name[256];
	};

	class obj_context
	{
	public:
		


		constexpr f32 scene_scale() const { return _scene_scale; }

	private:

		void load_obj_file(const char* file);
		void load_mtl_file(const char* mtl_file_path);

		scene*						_scene{ nullptr };
		scene_data*					_scene_data{ nullptr };
		f32							_scene_scale{ 1.f };
		utl::vector<mtl_config>		_mtl_config;
	};
}