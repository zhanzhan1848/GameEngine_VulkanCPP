#pragma once

#include "ToolsCommon.h"

namespace primal::tools {

	struct vertex
	{
		math::v4								tangent{};
		math::v4								joint_weights{};
		math::u32v4								joint_indices{ u32_invalid_id, u32_invalid_id, u32_invalid_id, u32_invalid_id };
		math::v3								position{};
		math::v3								normal{};
		math::v2								uv{};
		u8										red{}, green{}, blue{};
		u8										pad;
	};

	namespace elements
	{
		struct elements_type
		{
			enum type : u32
			{
				position_only					= 0x00,
				static_normal					= 0x01,
				static_normal_texture			= 0x03,
				static_color					= 0x04,
				skeletal						= 0x08,
				skeletal_color					= skeletal | static_color,
				skeletal_normal					= skeletal | static_normal,
				skeletal_normal_color			= skeletal_normal | static_color,
				skeletal_normal_texture			= skeletal | static_normal_texture,
				skeletal_normal_texture_color	= skeletal_normal_texture | static_color,
			};
		};

		struct static_color
		{
			u8									color[3];
			u8									pad;
		};

		struct static_normal
		{
			u8									color[3];
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									normal[2];
		};

		struct static_normal_texture
		{
			u8									color[3];
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									normal[2];
			u16									tangent[2];
			math::v2							uv;
		};

		struct skeletal
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									pad;
			u16									joint_indices[4];
		};
		
		struct skeletal_color
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									pad;
			u16									joint_indices[4];
			u8									color[3];
			u8									pad2;
		};

		struct skeletal_normal
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									joint_indices[4];
			u16									normal[2];
		};

		struct skeletal_normal_color
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									joint_indices[4];
			u16									normal[2];
			u8									color[3];
			u8									pad;
		};

		struct skeletal_normal_texture
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									joint_indices[4];
			u16									normal[2];
			u16									tangent[2];
			math::v2							uv;
		};

		struct skeletal_normal_texture_color
		{
			u8									joint_weithts[3]; // normailzed joint weights for up to 4 joints
			u8									t_sign;  // bit 0: tangent handedness * (tangent.z sign), bit 1: normal.z sign (0 means -1, 1 means +1).
			u16									joint_indices[4];
			u16									normal[2];
			u16									tangent[2];
			math::v2							uv;
			u8									color[3];
			u8									pad;
		};

	} // namespace elements

	struct mesh
	{
		// Initial data
		utl::vector<math::v3>					positions;
		utl::vector<math::v3>					normals;
		utl::vector<math::v4>					tangents;
		utl::vector<math::v3>					colors;
		utl::vector<utl::vector<math::v2>>		uv_sets;
		utl::vector<u32>						material_indices;
		utl::vector<u32>						material_used;

		u32										material_idx{ u32_invalid_id };
		utl::vector<u32>						raw_indices;

		// Meshlet data
		struct meshlet
		{
			u32 vertex_offset;
			u32 triangle_offset;
			u32 vertex_count;
			u32 triangle_count;
			
			f32 cone_apex[3];
			f32 cone_axis[3];
			f32 cone_cutoff;
			f32 center[3];
			f32 radius;
		};

		utl::vector<meshlet>					meshlets;
		utl::vector<u32>						meshlet_vertices;
		utl::vector<u8>							meshlet_triangles;

		// SDF data
		struct sdf_data
		{
			u32 resolution[3];
			f32 bounds_min[3];
			f32 bounds_max[3];
			utl::vector<u16> data; // half-float distance
			utl::vector<u8> voxels; // 8-bit occupancy/density
			utl::vector<u16> vector_field; // 4x half-float vector to nearest surface (RGBA)
		};
		sdf_data								sdf;

		// Intermediate data
		utl::vector<vertex>						vertices;
		utl::vector<u32>						indices;

		// Output data
		std::string								name;
		elements::elements_type::type			elements_type;
		utl::vector<u8>							position_buffer;
		utl::vector<u8>							element_buffer;

		f32										lod_threshold{ -1.f };
		u32										lod_id{ u32_invalid_id };
	};

	struct lod_group
	{
		std::string								name;
		utl::vector<mesh>						meshes;
	};

	struct material
	{
		std::string								name;
		std::string								diffuse_texture;
		std::string								normal_texture;
	};

	struct scene
	{
		std::string								name;
		utl::vector<lod_group>					lod_groups;
		utl::vector<material>					materials;
	};

	struct geometry_import_settings
	{
		f32										smoothing_angle;
		u8										calculate_normals;
		u8										calculate_tangents;
		u8										reverse_handedness;
		u8										import_embedded_textures;
		u8										import_animations;
		u8										coalesce_meshes;
	};

	struct  scene_data
	{
		u8*										buffer;
		u32										buffer_size;
		geometry_import_settings				settings;
	};

	void process_scene(scene& scene, const geometry_import_settings& settings, progression *const progression);
	void pack_data(const scene& scene, scene_data& data);
	bool coalesce_meshes(const lod_group& lod, mesh& combined_mesh, progression *const progression);
}