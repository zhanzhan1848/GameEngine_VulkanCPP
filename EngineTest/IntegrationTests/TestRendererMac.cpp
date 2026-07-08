#include "Platform/PlatformTypes.h"
#include "Platform/Platform.h"
#include "Graphics/Renderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
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

#if defined(_MSC_VER)
#include "Graphics/Direct3D12/D3D12Core.h"
#elif defined(__clang__)
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <simd/simd.h>

#endif

using namespace primal;

/// Multithreading test worker spawn code  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ENABLE_TEST_WORK 0

constexpr u32	num_threads{ 8 };
bool			shutdown{ false };
std::thread		workers[num_threads];
time_it 		timer{};
bool 			is_restarting{ false };

utl::vector<u8>	null_buffer(1024 * 1024, 0);
// Test worker for upload context

struct camera_surface
{
	game_entity::entity entity{};
	graphics::camera camera{};
	graphics::render_surface surface{};
};

primal::id::id_type item_id{ primal::id::invalid_id };
primal::id::id_type model_id{ primal::id::invalid_id };

camera_surface _surfaces[1]{};

primal::id::id_type create_render_item(primal::id::id_type entity_id);
void destory_render_item(primal::id::id_type item_id);
void get_render_items(primal::id::id_type* items, [[maybe_unused]] u32 count);
primal::id::id_type create_metarial(primal::id::id_type item_id);
void generate_lights();
void remove_lights();

game_entity::entity create_one_game_entity(math::v3 position, math::v3 rotation, const char* name)
{
	transform::init_info transform_info{};
    // 使用simd库创建四元数：先创建各轴的旋转四元数，然后相乘
    simd_quatf quat_y = simd_quaternion(rotation.y, simd_make_float3(0, 1, 0)); // yaw
	simd_quatf quat_x = simd_quaternion(rotation.x, simd_make_float3(1, 0, 0)); // pitch
	simd_quatf quat_z = simd_quaternion(rotation.z, simd_make_float3(0, 0, 1)); // roll

	// 注意右乘顺序：roll * pitch * yaw
	simd_quatf quat = simd_mul(quat_z, simd_mul(quat_x, quat_y));
    
    math::v4 rot_quat{
		quat.vector.x,
		quat.vector.y,
		quat.vector.z,
		quat.vector.w
	};
	memcpy(&transform_info.rotation[0], &rot_quat, sizeof(transform_info.rotation));
    memcpy(&transform_info.position[0], &position, sizeof(transform_info.position));

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

void create_camera_surface(camera_surface& surface, platform::window_init_info info)
{
	surface.surface.window = platform::create_window(&info);
	surface.surface.surface = graphics::create_surface(surface.surface.window);
	surface.entity = create_one_game_entity(math::v3{ 1.f, 1.f, -1.0f }, math::v3{ 0.f, 0.f, 1.f }, "camera_script");
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

bool test_initialize()
{
	script::initialize();

	// 尝试编译着色器，如果失败则自动重试几次
	constexpr int max_retries = 3;
	int retry_count = 0;
	
	while (!compile_shaders())
	{
		// 记录失败并增加重试计数
		std::cout << "Failed to compile engine shaders. Retry attempt " << (retry_count + 1) 
		          << " of " << max_retries << std::endl;
		
		retry_count++;
		if (retry_count >= max_retries)
		{
			std::cout << "Failed to compile shaders after " << max_retries << " attempts. Aborting." << std::endl;
			break;
		}
		
		// 短暂延迟后重试
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// === Phase 1 Sub-step 1.2.4': 三步初始化 ===
	// 1) initialize_with_device 创建 g_rhiDevice (rhi::MetalDevice)
	// 2) bind_rhi_device_to_legacy 把 native MTL::Device 注入 metal::core
	// 3) graphics::initialize(metal) 内部 create_device() 检测到 _external_device 跳过自动创建
	// 最终结果：旧 Metal 后端所有资源创建走 RHI 持有的 device
	graphics::rhi::DeviceDesc deviceDesc{};
	deviceDesc.platform = graphics::rhi::RHIPlatform::Metal;
	deviceDesc.enableDebug = false;
	if (!graphics::initialize_with_device(deviceDesc)) return false;
	if (!graphics::bind_rhi_device_to_legacy()) return false;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	if (!graphics::initialize(graphics::graphics_platform::metal)) return false;
#pragma GCC diagnostic pop

    platform::window_init_info info[]
	{
		{nullptr, nullptr, "Render Window 1", 100, 100, 1600, 900},
	};
	static_assert(_countof(info) == _countof(_surfaces));

	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		create_camera_surface(_surfaces[i], info[i]);

	
	// std::unique_ptr<u8[]> model;
	// u64 size{ 0 };
	// if (!read_file("model.model", model, size)) return false;

	// model_id = content::create_resource(model.get(), content::asset_type::mesh);
	// if (!primal::id::is_valid(model_id)) return false;

	// model shader material
	// auto material_id = create_metarial();

	item_id = create_metarial(create_one_game_entity(math::v3{ 0.f, 0.f, 0.f }, math::v3{ 0.f, 0.f, 0.f }, nullptr).get_id());
	assert(primal::id::is_valid(item_id));

	// Lights
	generate_lights();

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

	if (primal::id::is_valid(model_id))
	{
		content::destroy_resource(model_id, content::asset_type::mesh);
	}

	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
		destroy_camera_surface(_surfaces[i]);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	graphics::shutdown();
#pragma GCC diagnostic pop
	// === Phase 1 Sub-step 1.2.4': 关闭顺序 ===
	// 先 graphics::shutdown() (释放 metal::core 对 _external_device 的 retain + 所有 Metal 资源)
	// 再 shutdown_rhi() (释放 rhi::MetalDevice 对 mtlDevice_ 的所有权)
	graphics::shutdown_rhi();

	script::shutdown();
}

bool Engine_Test::initialize()
{
	return test_initialize();
}

void Engine_Test::run()
{
	[[maybe_unused]] static u32 counter{ 0 };
	static u32 light_set_key{ 0 };
	++counter;
	// if ((counter % 90) == 0) light_set_key = (light_set_key + 1) % 2;

	timer.begin();
	// std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const f32 dt{ timer.dt_avg() };
	script::update(dt);
	// test_lights(dt);
	for (u32 i{ 0 }; i < _countof(_surfaces); ++i)
	{
		if (_surfaces[i].surface.surface.is_valid())
		{
			f32 thresholds[3]{};

			graphics::frame_info info{};
			info.average_frame_time = dt;
			info.render_item_ids = &item_id;
			info.render_item_count = 1;
			info.thresholds = &thresholds[0];
			info.light_set_key = light_set_key;
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
	if (_displayLink) {
		CFRunLoopTimerInvalidate(_displayLink);
		CFRunLoopRemoveTimer(_runLoop, _displayLink, kCFRunLoopCommonModes);
		CFRelease(_displayLink);
		_displayLink = nullptr;
	}
}