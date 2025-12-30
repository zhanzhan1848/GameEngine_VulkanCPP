// 文件说明: RenderItem 测试与资源加载示例。
// 新增: 基于 ECS 的 MeshComponent 用法示例函数，展示 CPU→GPU 的几何资源绑定与渲染项创建。
// 平台: MacOS，渲染后端: Metal-CPP（框架参照 D3D12）。
#include <filesystem>
#include "CommonHeaders.h"
#include "Content//ContentToEngine.h"
#include "Graphics/Renderer.h"
#include "ShaderCompilation.h"
#include "Components/Entity.h"
#include "Components/Mesh.h"
#include "../ContentTools/Geometry.h"

#include <thread>

using namespace primal;

bool read_file(std::filesystem::path, std::unique_ptr<u8[]>&, u64&);

namespace 
{
	id::id_type model_id{ id::invalid_id };
	id::id_type dp_vs_id{ id::invalid_id };
	id::id_type dp_ps_id{ id::invalid_id };
	id::id_type vs_id{ id::invalid_id };
	id::id_type ps_id{ id::invalid_id };
	id::id_type textured_ps_id{ id::invalid_id };
	id::id_type dp_mtl_id{ id::invalid_id };
	id::id_type mtl_id{ id::invalid_id };
	id::id_type textured_mtl_id{ id::invalid_id };

	struct texture_usage 
	{
		enum usage : u32 
		{
			ambient_occlusion = 0,
			base_color,
			emissive,
			metal_rough,
			normal,

			count
		};
	};

	id::id_type texture_ids[texture_usage::count];
	
	std::unordered_map<id::id_type, id::id_type> render_item_entity_map;

	[[nodiscard]] id::id_type load_model(const char* path)
	{
		// load test model
		std::unique_ptr<u8[]> model;
		u64 size{ 0 };
		read_file(path, model, size);

		const id::id_type model_id{ content::create_resource(model.get(), content::asset_type::mesh) };
		assert(id::is_valid(model_id));
		return model_id;
	}

	[[nodiscard]] id::id_type load_texture(const char* path)
	{
		// load test texture
		std::unique_ptr<u8[]> texture;
		u64 size{ 0 };
		read_file(path, texture, size);

		const id::id_type texture_id{ content::create_resource(texture.get(), content::asset_type::texture) };
		assert(id::is_valid(texture_id));
		return texture_id;
	}

	void load_model()
	{
		std::unique_ptr<u8[]> model;
		u64 size{ 0 };
		read_file("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/model_win_engine.model", model, size);

		model_id = content::create_resource(model.get(), content::asset_type::mesh);
		assert(id::is_valid(model_id));
	}

	void load_shaders()
	{
		// let's say our material uses a vertex shader and a pixel shader
		shader_file_info info{};
		info.file_name = "TestShader.hlsl";
		info.function = "TestShaderVS";
		info.type = shader_type::vertex;

		const char* shader_path{ "../EngineTest/" };

		std::wstring defines[]{ L"ELEMENT_TYPE=1", L"ELEMENT_TYPE=3" };
		utl::vector<u32> keys;
		keys.emplace_back(tools::elements::elements_type::skeletal_normal);
		keys.emplace_back(tools::elements::elements_type::skeletal_normal_texture);

		utl::vector<std::wstring> extra_args{};
		utl::vector<std::unique_ptr<u8[]>> vertex_shaders;
		utl::vector<u8*> vertex_shader_pointers;
		for (u32 i{ 0 }; i < _countof(defines); ++i)
		{
			extra_args.clear();
			extra_args.emplace_back(L"-D");
			vertex_shaders.emplace_back(std::move(compile_shader(info, shader_path, extra_args)));
			assert(vertex_shaders.back().get());
			vertex_shader_pointers.emplace_back(vertex_shaders.back().get());
		}
		extra_args.clear();

		info.function = "TestShaderPS";
		info.type = shader_type::pixel;
		utl::vector<std::unique_ptr<u8[]>> pixel_shaders;

		pixel_shaders.emplace_back(compile_shader(info, shader_path, extra_args));
		assert(pixel_shaders.back().get());

		defines[0] = L"TEXTURED_MTL=1";
		extra_args.emplace_back(L"-D");
		extra_args.emplace_back(defines[0]);

		pixel_shaders.emplace_back(compile_shader(info, shader_path, extra_args));
		assert(pixel_shaders.back().get());

		vs_id = content::add_shader_group(vertex_shader_pointers.data(), (u32)vertex_shader_pointers.size(), keys.data());

		const u8* pixel_shader_pointer[]{ pixel_shaders[0].get()};
		ps_id = content::add_shader_group(pixel_shader_pointer, 1, &u32_invalid_id);

		pixel_shader_pointer[0] = pixel_shaders[1].get();
		textured_ps_id = content::add_shader_group(pixel_shader_pointer, 1, &u32_invalid_id);
	}

	void load_shaders_metal_depth_pass()
	{
		const char* shader_path{ "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/" };  // "../EngineTest"

#pragma region DepthPassShader.metal
		shader_file_info info{};
		info.file_name = "DepthPassShader.metal";
		info.function = "depth_pass_vertex_main";
		info.type = shader_type::vertex;

		utl::vector<std::wstring> dp_extra_args{};
		utl::vector<std::unique_ptr<u8[]>> dp_vertex_shaders;
		utl::vector<u8*> dp_vertex_shader_pointers;
		utl::vector<u32> keys;
		keys.emplace_back(tools::elements::elements_type::static_normal_texture);
		
		dp_vertex_shaders.emplace_back(std::move(compile_shader(info, shader_path, dp_extra_args)));
		assert(dp_vertex_shaders.back().get());
		dp_vertex_shader_pointers.emplace_back(dp_vertex_shaders.back().get());

		dp_vs_id = content::add_shader_group(dp_vertex_shader_pointers.data(), (u32)dp_vertex_shader_pointers.size(), keys.data());
		content::add_shader_function_name(dp_vs_id, info.function);

		info.function = "depth_pass_fs_main";
		info.type = shader_type::pixel;
		utl::vector<std::unique_ptr<u8[]>> dp_pixel_shaders;
		dp_pixel_shaders.emplace_back(compile_shader(info, shader_path, dp_extra_args));
		assert(dp_pixel_shaders.back().get());
		const u8* dp_pixel_shader_pointer[]{ dp_pixel_shaders[0].get()};
		dp_ps_id = content::add_shader_group(dp_pixel_shader_pointer, 1, &u32_invalid_id);
		content::add_shader_function_name(dp_ps_id, info.function);
#pragma endregion
	}

	void load_shaders_metal_gpass()
	{
		const char* shader_path{ "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/" };  // "../EngineTest"
		shader_file_info info{};
#pragma region TestShader.metal
		info.file_name = "TestShader.metal";
		info.function = "vertex_main";
		info.type = shader_type::vertex;

		utl::vector<std::wstring> dp_extra_args{};
		utl::vector<std::unique_ptr<u8[]>> dp_vertex_shaders;
		utl::vector<u8*> dp_vertex_shader_pointers;
		utl::vector<u32> keys;

		std::wstring defines[]{ L"ELEMENT_TYPE=1" };
		// keys.emplace_back(tools::elements::elements_type::skeletal_normal);
		keys.emplace_back(tools::elements::elements_type::static_normal_texture);

		utl::vector<std::wstring> extra_args{};
		utl::vector<std::unique_ptr<u8[]>> vertex_shaders;
		utl::vector<u8*> vertex_shader_pointers;
		for (u32 i{ 0 }; i < _countof(defines); ++i)
		{
			extra_args.clear();
			// extra_args.emplace_back(L"-D");
			vertex_shaders.emplace_back(std::move(compile_shader(info, shader_path, extra_args)));
			assert(vertex_shaders.back().get());
			vertex_shader_pointers.emplace_back(vertex_shaders.back().get());
		}
		extra_args.clear();

		info.function = "fragment_main";
		info.type = shader_type::pixel;
		utl::vector<std::unique_ptr<u8[]>> pixel_shaders;

		pixel_shaders.emplace_back(compile_shader(info, shader_path, extra_args));
		assert(pixel_shaders.back().get());

		defines[0] = L"TEXTURED_MTL=1";
		// extra_args.emplace_back(L"-D");
		// extra_args.emplace_back(defines[0]);

		pixel_shaders.emplace_back(compile_shader(info, shader_path, extra_args));
		assert(pixel_shaders.back().get());

		vs_id = content::add_shader_group(vertex_shader_pointers.data(), (u32)vertex_shader_pointers.size(), keys.data());
		content::add_shader_function_name(vs_id, "vertex_main");

		const u8* pixel_shader_pointer[]{ pixel_shaders[0].get()};
		ps_id = content::add_shader_group(pixel_shader_pointer, 1, &u32_invalid_id);
		content::add_shader_function_name(ps_id, "fragment_main");

		// pixel_shader_pointer[0] = pixel_shaders[1].get();
		// textured_ps_id = content::add_shader_group(pixel_shader_pointer, 1, &u32_invalid_id);
		// content::add_shader_function_name(textured_ps_id, "fragment_main");
#pragma endregion
	}

	void create_material()
	{
		graphics::material_init_info info{};
#pragma region DepthPassShader.metal
		// info.shader_ids[graphics::shader_type::vertex] = dp_vs_id;
		// info.shader_ids[graphics::shader_type::pixel] = dp_ps_id;
		// info.type = graphics::material_type::opauqe;
		// dp_mtl_id = content::create_resource(&info, content::asset_type::material);
#pragma endregion

#pragma region TestShader.metal
		info.shader_ids[graphics::shader_type::vertex] = vs_id;
		info.shader_ids[graphics::shader_type::pixel] = ps_id;
		info.type = graphics::material_type::opauqe;
		mtl_id = content::create_resource(&info, content::asset_type::material);
#pragma endregion

		// info.shader_ids[graphics::shader_type::pixel] = textured_ps_id;
		// info.texture_count = 1; //texture_usage::count;
		// info.texture_ids = &texture_ids[0];
		// textured_mtl_id = content::create_resource(&info, content::asset_type::material);
	}

} // anonymous namespace

id::id_type create_metarial(id::id_type entity_id)
{

	memset(&texture_ids[0], 0xff, sizeof(id::id_type) * _countof(texture_ids));

	std::thread threads[]
	{
		std::thread{ [] { texture_ids[0] = load_texture("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/fbx_textures_encode/Lion_Albedo_1.asset"); } },
		// std::thread{ [] { load_shaders_metal_depth_pass(); } },
		std::thread{ [] { load_shaders_metal_gpass(); } },
		std::thread{ [] { load_model();  } }
	};

	for (auto& t : threads)
	{
		t.join();
	}

	create_material();
	id::id_type materials[]{ mtl_id, mtl_id }; // , textured_mtl_id

	id::id_type item_id{ graphics::add_render_item(entity_id, model_id, _countof(materials), &materials[0]) };
	render_item_entity_map[item_id] = entity_id;

	return item_id;
}

id::id_type create_render_item(id::id_type entity_id)
{

	memset(&texture_ids[0], 0xff, sizeof(id::id_type) * _countof(texture_ids));

	std::thread threads[]{
		std::thread{ [] { model_id = load_model("..\\..\\x64\\model.model"); } },
		std::thread{ [] { texture_ids[texture_usage::ambient_occlusion] = load_texture("..\\..\\x64\\texture.texture"); }},
		std::thread{ [] { load_shaders();  } }
	};

	for (auto& t : threads)
	{
		t.join();
	}

	// add a render item using the model and its materials
	create_material();
	id::id_type materials[]{ mtl_id };

	// TODO: add add_rendeer_item in renderer.
	id::id_type item_id{ graphics::add_render_item(entity_id, model_id, _countof(materials), &materials[0]) };

	render_item_entity_map[item_id] = entity_id;

	return item_id;
}

// 函数说明: 使用 ECS MeshComponent 创建实体并在创建时绑定几何与材质。
// 步骤概要:
// 1) 并行加载模型与着色器，创建材质；
// 2) 构造 Transform 与 Mesh 组件初始化信息；
// 3) 通过 game_entity::create 创建实体（内部自动创建渲染项，无需显式调用 graphics::add_render_item）。
// 返回: 新建实体的 id（与 MeshComponent 的 id 对齐）。
// id::id_type create_entity_with_mesh_component_example()
// {
//     // 1) 并行加载资源（模型 + 着色器），随后创建材质
//     memset(&texture_ids[0], 0xff, sizeof(id::id_type) * _countof(texture_ids));

//     std::thread threads[]{
//         std::thread{ [] { load_shaders_metal_gpass(); } },
//         std::thread{ [] { load_model(); } }
//     };
//     for (auto& t : threads) { t.join(); }
//     create_material();

//     // 2) 组件初始化信息：Transform + Mesh
//     transform::init_info tinfo{};
//     tinfo.position[0] = 0.f; tinfo.position[1] = 0.f; tinfo.position[2] = 0.f;
//     tinfo.rotation[0] = 0.f; tinfo.rotation[1] = 0.f; tinfo.rotation[2] = 0.f; tinfo.rotation[3] = 1.f;
//     tinfo.scale[0] = 1.f; tinfo.scale[1] = 1.f; tinfo.scale[2] = 1.f;

//     id::id_type materials[]{ mtl_id };
//     mesh::init_info minfo{};
//     minfo.geometry_content_id = model_id;        // Content 层创建的几何资源 id
//     minfo.material_ids = &materials[0];          // 与几何 submesh 顺序一致
//     minfo.material_count = _countof(materials);  // 本示例仅一个子网格/材质
//     minfo.keep_cpu_copy = true;                  // 如需后续 CPU 优化与重上传
//     minfo.lod_bias = 0.f;                        // LOD 策略示例参数
//     minfo.forced_lod = -1;                       // -1: 自动选择

//     game_entity::entity_info einfo{};
//     einfo.transform = &tinfo;
//     einfo.script = nullptr;
//     einfo.mesh = &minfo;

//     // 3) 创建实体（内部完成 Transform/Mesh 组件创建与渲染项绑定）
//     const game_entity::entity e = game_entity::create(einfo);
//     assert(e.is_valid());
//     return id::id_type(e.get_id());
// }

void destory_render_item(id::id_type item_id)
{
	// remove the render item from engine (also the game entity)
	if (id::is_valid(item_id))
	{
		graphics::remove_render_item(item_id);
		auto pair = render_item_entity_map.find(item_id);
		if (pair != render_item_entity_map.end())
		{
			game_entity::remove(game_entity::entity_id{ pair->second });
		}
	}

	// remove material
	if (id::is_valid(mtl_id))
	{
		content::destroy_resource(mtl_id, content::asset_type::material);
	}

	if (id::is_valid(textured_mtl_id))
	{
		content::destroy_resource(textured_mtl_id, content::asset_type::material);
	}

	// remove textures
	for (id::id_type id : texture_ids)
	{
		if (id::is_valid(id))
		{
			content::destroy_resource(id, content::asset_type::texture);
		}
	}

	// remove shaders
	if (id::is_valid(vs_id))
	{
		content::remove_shader_group(vs_id);
	}

	if (id::is_valid(ps_id))
	{
		content::remove_shader_group(ps_id);
	}

	if (id::is_valid(textured_ps_id))
	{
		content::remove_shader_group(textured_ps_id);
	}

	// remove model
	if (id::is_valid(model_id))
	{
		content::destroy_resource(model_id, content::asset_type::mesh);
	}
}

void get_render_items(id::id_type* items, [[maybe_unused]] u32 count)
{
	assert(count != 0);
	items[0] = 0;
}