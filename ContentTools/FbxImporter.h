#pragma once

#include "ToolsCommon.h"
#include <fbxsdk.h>
#include <cstring>
#include <unordered_map>

namespace primal::tools
{
	struct scene_data;
	struct scene;
	struct mesh;
	struct geeometry_import_settings;

	class fbx_context
	{
	public:
		fbx_context(const char* file, scene* scene, scene_data* data, progression *const progression)
			: _scene{ scene }, _scene_data{ data }, _progression{ progression }
		{
			assert(file && _scene && _scene_data && _progression);
			if (initialize_fbx())
			{
				load_fbx_file(file);
				assert(is_valid());
			}
		}

		~fbx_context()
		{
			_fbx_scene->Destroy();
			_fbx_manager->Destroy();
			// [R12/Windows] 原实现在此 ZeroMemory/memset 整个对象：非平凡成员
			// _material_map（std::unordered_map）的内部指针被一并清零，函数体
			// 结束后成员自动析构在已清零的 map 上运行——MSVC 下 0xC0000005
			// （Windows 端 cook-mesh 必崩），libc++ 的零化 map 析构恰好 no-op
			// 故 macOS 未暴露；双平台共有的 UB。整体清零改为仅逐个 POD 成员
			// 置零（保持擦除指针的防御意图），_material_map 交由自然析构清理，
			// 绝不 memset 非平凡类型所在的整个对象。
			_fbx_scene = nullptr;
			_fbx_manager = nullptr;
			_scene = nullptr;
			_scene_data = nullptr;
			_progression = nullptr;
			_scene_scale = 0.f;
		}

		void get_scene(FbxNode* root = nullptr);

		constexpr bool is_valid() const { return _fbx_manager && _fbx_scene; }
		constexpr f32 scene_scale() const { return _scene_scale; }
		constexpr progression* get_progression() const { return _progression; }

	private:

		bool initialize_fbx();
		void load_fbx_file(const char* file);
		void get_meshes(FbxNode* node, utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
		void get_mesh(FbxNodeAttribute* attribute, utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold);
		void get_lod_group(FbxNodeAttribute* atrtribute);
		bool get_mesh_data(FbxMesh* fbx_mesh, mesh& m);
		void process_materials();

		scene*							_scene{ nullptr };
		scene_data*						_scene_data{ nullptr };
		FbxManager*						_fbx_manager{ nullptr };
		FbxScene*						_fbx_scene{ nullptr };
		progression*					_progression{ nullptr };
		f32								_scene_scale{ 1.0f };
		std::unordered_map<FbxSurfaceMaterial*, u32> _material_map;
	};
}