#include "Platform/PlatformTypes.h"
#include "Platform/Platform.h"
#include "Graphics/Renderer.h"
#include "Graphics/Direct3D12/D3D12Core.h"
#include "Content/ContentToEngine.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Script.h"
#include "Input/Input.h"
#include "TestRenderer.h"
#include "ShaderCompilation.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "PythonScript.h"

#if TEST_RENDERER

using namespace primal;

/// Multithreading test worker spawn code  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ENABLE_TEST_WORK 0

constexpr u32	num_threads{ 8 };
bool			shutdown{ false };
std::thread		workers[num_threads];

utl::vector<u8>	null_buffer(1024 * 1024, 0);
// Test worker for upload context
void buffer_test_worker()
{
	while(!shutdown)
	{
		auto* resource = graphics::d3d12::d3dx::create_buffer(null_buffer.data(), (u32)null_buffer.size());
		// NOTE: We can also use core::release(resource) since we're not using the buffer for rendering,
		//		 However, this is a nice test for deferred_release functionality.
		graphics::d3d12::core::deferred_release(resource);
	}
}

template<class FnPtr, class... Args>
void init_test_workers([[maybe_unused]] FnPtr&& fnPtr, [[maybe_unused]] Args&&... args)
{
#if ENABLE_TEST_WORK
	shutdown = false;
	for (auto& w : workers)
		w = std::thread(std::forward<FnPtr>(fnPtr), std::forward<Args>(args)...);
#endif
}

void join_test_workers()
{
#if ENABLE_TEST_WORK
	shutdown = true;
	for (auto& w : workers) w.join();
#endif
}
/// </summary>
struct camera_surface
{
	game_entity::entity entity{};
	graphics::camera camera{};
	graphics::render_surface surface{};
};

id::id_type item_id{ id::invalid_id };
id::id_type model_id{ id::invalid_id };

camera_surface _surfaces[1]{};
time_it timer{};

bool resized{ false };
bool is_restarting{ false };
void destroy_camera_surface(camera_surface& surface);
bool test_initialize();
void test_shutdown();
id::id_type create_render_item(id::id_type entity_id);
void destory_render_item(id::id_type item_id);
void generate_lights();
void remove_lights();
void test_lights(f32 dt);

LRESULT win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	bool toggle_fullscreen{ false };

	switch (msg)
	{
	case WM_DESTROY:
	{
		bool all_closed{ true };
		for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		{
			if (_surfaces[i].surface.window.is_valid())
			{
				if (_surfaces[i].surface.window.is_closed())
				{
					destroy_camera_surface(_surfaces[i]);
				}
				else
				{
					all_closed = false;
				}
			}
		}
		if (all_closed && !is_restarting)
		{
			PostQuitMessage(0);
			return 0;
		}
	}
	break;
	case WM_SIZE:
		resized = wparam != SIZE_MINIMIZED;
		break;
	case WM_SYSCHAR:
		toggle_fullscreen = (wparam == VK_RETURN && (HIWORD(lparam) & KF_ALTDOWN));
		break;
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE)
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return 0;
		}
		else if (wparam == VK_F11)
		{
			is_restarting = true;
			test_shutdown();
			test_initialize();
		}
	}

	if ((resized && GetKeyState(VK_LBUTTON) >= 0) || toggle_fullscreen)
	{
		platform::window win{ platform::window_id{ (id::id_type)GetWindowLongPtr(hwnd, GWLP_USERDATA) } };
		for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		{
			if (win.get_id() == _surfaces[i].surface.window.get_id())
			{
				if (toggle_fullscreen)
				{
					win.set_fullscreen(!win.is_fullscreen());
					// The default window procedure will play a system notification sound
					// when pressing the ALT+Enter keyboard combination if WM_SYSCHAR is
					// not handle. By returning 0 we can tell the system that we handled
					// this message.
					return 0;
				}
				else
				{
					_surfaces[i].surface.surface.resize(win.width(), win.height());
					_surfaces[i].camera.aspect_ratio((f32)win.width() / win.height());

					resized = false;
				}
				break;
			}
		}
	}

	return DefWindowProc(hwnd, msg, wparam, lparam);
}

game_entity::entity create_one_game_entity(math::v3 position, math::v3 rotation, const char* name)
{
	transform::init_info transform_info{};
	DirectX::XMVECTOR quat{ DirectX::XMQuaternionRotationRollPitchYawFromVector(DirectX::XMLoadFloat3(&rotation)) };
	math::v4a rot_quat;
	DirectX::XMStoreFloat4A(&rot_quat, quat);
	memcpy(&transform_info.rotation[0], &rot_quat.x, sizeof(transform_info.rotation));
	memcpy(&transform_info.position[0], &position.x, sizeof(transform_info.position));

	script::init_info script_info{};
	if (name)
	{
		script_info.script_creator = script::detail::get_script_creator(script::detail::string_hash()(name));
		assert(script_info.script_creator);
	}

	game_entity::entity_info entity_info{};
	entity_info.transform = &transform_info;
	entity_info.script = &script_info;
	game_entity::entity ntt{ game_entity::create(entity_info) };
	assert(ntt.is_valid());
	return ntt;
}

void remove_game_entity(game_entity::entity_id id)
{
	game_entity::remove(id);
}

bool read_file(std::filesystem::path path, std::unique_ptr<u8[]>& data, u64& size)
{
	if (!std::filesystem::exists(path)) return false;

	size = std::filesystem::file_size(path);
	assert(size);
	if (!size) return false;
	data = std::make_unique<u8[]>(size);
	std::ifstream file{ path, std::ios::in | std::ios::binary };
	if (!file || !file.read((char*)data.get(), size))
	{
		file.close();
		return false;
	}
	file.close();
	return true;
}

void create_camera_surface(camera_surface& surface, platform::window_init_info info)
{
	surface.surface.window = platform::create_window(&info);
	surface.surface.surface = graphics::create_surface(surface.surface.window);
	surface.entity = create_one_game_entity({ 2.f, 0.8f, 1.0f }, { 0.0f, 0.0f, 1.0f }, "camera_script");
	surface.camera = graphics::create_camera(graphics::perspective_camera_init_info{ surface.entity.get_id() });
	surface.camera.aspect_ratio((f32)surface.surface.window.width() / surface.surface.window.height());
}

void destroy_camera_surface(camera_surface& surface)
{
	camera_surface temp{ surface };
	surface = {};
	if(temp.surface.surface.is_valid()) graphics::remove_surface(temp.surface.surface.get_id());
	if(temp.surface.window.is_valid()) platform::remove_window(temp.surface.window.get_id());
	if(temp.camera.is_valid()) graphics::remove_camera(temp.camera.get_id());
	if (temp.entity.is_valid()) game_entity::remove(temp.entity.get_id());
}

bool test_initialize()
{
#define GRAPHICS_API graphics::graphics_platform::vulkan_1

	if constexpr (GRAPHICS_API == graphics::graphics_platform::direct3d12)
	{
		while (!compile_shaders())
		{
			// Pop up a message box allowing the user to retry compilation.
			if (MessageBox(nullptr, L"Failed to compile engine shaders.", L"Shader Compilation Error", MB_RETRYCANCEL) != IDRETRY)
				return false;
		}
	}
	else if constexpr(GRAPHICS_API == graphics::graphics_platform::vulkan_1)
	{ 
		typedef void(*ptrSub)(const char*, const char*);
		HMODULE hMod = LoadLibraryA("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/x64/DebugEditor/ContentTools.dll");
		ptrSub obj_loader = (ptrSub)GetProcAddress(hMod, "ImportObj");
		ptrSub single_obj_loader = (ptrSub)GetProcAddress(hMod, "ImportSimgleObj");
		if (obj_loader != nullptr)
		{
			std::filesystem::path kms_file{"C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/EngineTest/assets/kms/sponza/shaders"};
			if (!std::filesystem::exists(kms_file))
			{
				obj_loader("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/EngineTest/assets/models/sponza.obj", "sponza");
				single_obj_loader("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/EngineTest/assets/models/EngineSphere.obj", "sphere");
			}
		}

		//system("python C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Python_Scripts/compileShaders.py");
		pyscript();
	}
	else
	{ }

	if (!graphics::initialize(GRAPHICS_API)) return false;


	platform::window_init_info info[]
	{
		{&win_proc, nullptr, L"Render Window 1", 100, 100, 1600, 900},
		//{&win_proc, nullptr, L"Render Window 2", 150, 150, 800, 400},
		//{&win_proc, nullptr, L"Render Window 3", 200, 200, 400, 400},
		//{&win_proc, nullptr, L"Render Window 4", 250, 250, 800, 600},
	};
	static_assert(_countof(info) == _countof(_surfaces));

	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		create_camera_surface(_surfaces[i], info[i]);

	if constexpr (GRAPHICS_API == graphics::graphics_platform::vulkan_1)
	{
		// load test model
		std::unique_ptr<u8[]> model;
		u64 size{ 0 };
		// if (!read_file("model.model", model, size)) return false;

		//model_id = content::create_resource(model.get(), content::asset_type::mesh);
		// if (!id::is_valid(model_id)) return false;

		// init_test_workers(buffer_test_worker);

		//item_id = create_render_item(create_one_game_entity(false).get_id());

		generate_lights();
	}
	else
	{
		// load test model
		std::unique_ptr<u8[]> model;
		u64 size{ 0 };
		 if (!read_file("model.model", model, size)) return false;

		model_id = content::create_resource(model.get(), content::asset_type::mesh);
		 if (!id::is_valid(model_id)) return false;

		 init_test_workers(buffer_test_worker);

		 item_id = create_render_item(create_one_game_entity({}, {}, nullptr).get_id());

		 generate_lights();
	}

	input::input_source source{};
	source.binding = std::hash<std::string>()("move");
	source.source_type = input::input_source::keyboard;
	source.code = input::input_code::key_a;
	source.multiplier = 1.f;
	source.axis = input::axis::x;
	input::bind(source);

	source.code = input::input_code::key_d;
	source.multiplier = -1.f;
	input::bind(source);

	source.code = input::input_code::key_w;
	source.multiplier = 1.f;
	source.axis = input::axis::z;
	input::bind(source);

	source.code = input::input_code::key_s;
	source.multiplier = -1.f;
	input::bind(source);

	source.code = input::input_code::key_q;
	source.multiplier = -1.f;
	source.axis = input::axis::y;
	input::bind(source);

	source.code = input::input_code::key_e;
	source.multiplier = 1.f;
	input::bind(source);

	is_restarting = false;
	return true;
}

void test_shutdown()
{
	remove_lights();
	input::unbind(std::hash<std::string>()("move"));
	destory_render_item(item_id);
	join_test_workers();

	if (id::is_valid(model_id))
	{
		content::destroy_resource(model_id, content::asset_type::mesh);
	}

	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		destroy_camera_surface(_surfaces[i]);

	graphics::shutdown();
}

bool Engine_Test::initialize()
{
	return test_initialize();
}

void Engine_Test::run()
{
	static u32 counter{ 0 };
	static u32 light_set_key{ 0 };
	++counter;
	// if ((counter % 90) == 0) light_set_key = (light_set_key + 1) % 2;

	timer.begin();
	//std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const f32 dt{ timer.dt_avg() };
	script::update(dt);
	// test_lights(dt);
	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
	{
		if (_surfaces[i].surface.surface.is_valid())
		{
			f32 thresholds[3]{};

			graphics::frame_info info{};
			info.render_item_ids = &item_id;
			info.render_item_count = 1;
			info.thresholds = &thresholds[0];
			info.light_set_key = light_set_key;
			info.average_frame_time = dt;
			info.camera_id = _surfaces[i].camera.get_id();

			assert(_countof(thresholds) >= info.render_item_count);
			_surfaces[i].surface.surface.render(info);
		}
	}
	timer.end();
}

void Engine_Test::shutdown()
{
	test_shutdown();
}


#endif // TEST_RENDERER