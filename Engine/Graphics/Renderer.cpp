#include "Renderer.h"
#include "GraphicsPlatformInterface.h"
#include "Direct3D12//D3D12Interface.h"
#include "Vulkan/VulkanInterface.h"
#if defined(__APPLE__)
#include "Metal/MetalInterface.h"
#endif
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#if defined(__APPLE__)
#include "Metal/MetalCore.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#endif

// === Phase 1 Sub-step 1.2.6': Renderer.cpp = 旧 platform_interface 转发层 ===
// 整个 Renderer.cpp 的 forwarding 逻辑（gfx.surface.create / gfx.light.set_parameter / ...）
// 都通过静态 platform_interface gfx{} 变量做后端分发。这条路径已被 RHI device path
// （initialize_with_device + bind_rhi_device_to_legacy + initialize(metal)）替代，
// 但 forwarding API 本身还是 public surface（Editor 和集成测试在用）。
// 所以保留实现，只抑制整个 TU 的 deprecation 警告。Phase 2 会真正删除 gfx 分发路径。
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace primal::graphics
{
	namespace
	{

		// Defines where the compiled engine shaders file is located for each one of the supported APIs.
		constexpr const char* engine_shader_paths[]
		{
			"./shaders/d3d12/shaders.bin",
			"./shaders/vulkan/shaders.bin", 
			"/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Darwin/Debug/shaders/metal/shaders.metallib", // "./shaders/metal/shaders.metallib"
			// "./shaders/vulkan/shaders.bin", 
			//etc
		};

		platform_interface gfx{};

		// === RHI 路径状态（新）===
		// 与 platform_interface gfx{} 并存。initialize_with_device 后此非空。
		rhi::RHIDeviceBase* g_rhiDevice{ nullptr };

#ifndef PRIMAL_PLUS

		bool set_platform_interface(graphics_platform platform, platform_interface& pi)
		{
			switch (platform)
			{
			case graphics_platform::direct3d12:
				d3d12::get_platform_interface(gfx);
				break;
			case graphics_platform::vulkan_1:
				vulkan::get_platform_interface(gfx);
				break;
#if defined(__APPLE__)
			case graphics_platform::metal:
				metal::get_platform_interface(gfx);
				break;
#endif
			default:
				return false;
			}
			assert(gfx.platform == platform);
			return true;
		}

#endif // !PRIMAL_PLUS
		
	} // anonymous namespace

#ifdef PRIMAL_PLUS
	extern bool set_platform_interface(graphics_platform platform, platform_interface& pi);
#endif // 


	bool initialize(graphics_platform platform)
	{
		return set_platform_interface(platform, gfx) && gfx.initialize();
	}

	void shutdown()
	{
		if(gfx.platform != (graphics_platform) - 1) gfx.shutdown();
	}

	const char* get_engine_shaders_path()
	{
		return engine_shader_paths[(u32)gfx.platform];
	}

	const char* get_engine_shaders_path(graphics_platform platform)
	{
		return engine_shader_paths[(u32)platform];
	}

	surface create_surface(platform::window window)
	{
		return gfx.surface.create(window);
	}

	void remove_surface(surface_id id)
	{
		assert(id::is_valid(id));
		gfx.surface.remove(id);
	}

	void surface::resize(u32 width, u32 height) const
	{
		assert(is_valid());
		gfx.surface.resize(_id, width, height);
	}

	u32 surface::width() const
	{
		assert(is_valid());
		return gfx.surface.width(_id);
	}

	u32 surface::height() const
	{
		assert(is_valid());
		return gfx.surface.height(_id);
	}

	void surface::render(frame_info info) const
	{
		assert(is_valid());
		gfx.surface.render(_id, info);
	}

	u32 surface::blit_and_present(rhi::ResourceHandle src) const
	{
		assert(is_valid());
		// gfx.surface.blit_and_present may be null on backends that don't
		// implement Path B (e.g. D3D12/Vulkan stubs). Guard the deref so a
		// missing impl degrades to a no-op return 0 instead of crashing.
		if (!gfx.surface.blit_and_present) return 0;
		return gfx.surface.blit_and_present(_id, src);
	}

	void create_light_set(u64 light_set_key)
	{
		gfx.light.create_light_set(light_set_key);
	}

	void remove_light_set(u64 light_set_key)
	{
		gfx.light.remove_light_set(light_set_key);
	}

#if defined(_MSC_VER)
	light create_light(light_init_info info)
	{
		return gfx.light.create(info);
	}
#elif defined(__clang__)
	light create_light(light_init_info& info)
	{
		return gfx.light.create(info);
	}
#endif

	void remove_light(light_id id, u64 light_set_key)
	{
		gfx.light.remove(id, light_set_key);
	}

	void light::is_enabled(bool is_enabled) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::is_enabled, &is_enabled, sizeof(is_enabled));
	}

	void light::intensity(f32 intensity) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::intensity, &intensity, sizeof(intensity));
	}

	void light::color(math::v3 color) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::color, &color, sizeof(color));
	}

	void light::attenuation(math::v3 attenuation) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::attenuation, &attenuation, sizeof(attenuation));
	}

	void light::range(f32 range) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::range, &range, sizeof(range));
	}

	void light::cone_angles(f32 umbra, f32 penumbra) const
	{
		assert(is_valid());
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::umbra, &umbra, sizeof(umbra));
		gfx.light.set_parameter(_id, _light_set_key, light_parameter::penumbra, &penumbra, sizeof(penumbra));
	}

	bool light::is_enabled() const
	{
		assert(is_valid());
		bool is_enabled;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::is_enabled, &is_enabled, sizeof(is_enabled));
		return is_enabled;
	}

	f32 light::intensity() const
	{
		assert(is_valid());
		f32 intensity;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::intensity, &intensity, sizeof(intensity));
		return intensity;
	}

	math::v3 light::color() const
	{
		assert(is_valid());
		math::v3 color;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::color, &color, sizeof(color));
		return color;
	}

	math::v3 light::attenuation() const
	{
		assert(is_valid());
		math::v3 attenuation;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::attenuation, &attenuation, sizeof(attenuation));
		return attenuation;
	}

	f32 light::range() const
	{
		assert(is_valid());
		f32 range;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::range, &range, sizeof(range));
		return range;
	}

	f32 light::umbra() const
	{
		assert(is_valid());
		f32 umbra;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::umbra, &umbra, sizeof(umbra));
		return umbra;
	}

	f32 light::penumbra() const
	{
		assert(is_valid());
		f32 penumbra;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::penumbra, &penumbra, sizeof(penumbra));
		return penumbra;
	}

	light::type light::light_type() const
	{
		assert(is_valid());
		type type;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::type, &type, sizeof(type));
		return type;
	}

	id::id_type light::entity_id() const
	{
		assert(is_valid());
		id::id_type id;
		gfx.light.get_parameter(_id, _light_set_key, light_parameter::entity_id, &id, sizeof(id));
		return id;
	}

	camera create_camera(camera_init_info info)
	{
		return gfx.camera.create(info);
	}

	void remove_camera(camera_id id)
	{
		gfx.camera.remove(id);
	}

	void camera::up(math::v3 up) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::up_vector, &up, sizeof(up));
	}

	void camera::field_of_view(f32 fov) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::field_of_view, &fov, sizeof(fov));
	}

	void camera::aspect_ratio(f32 aspect_ratio) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::aspect_ratio, &aspect_ratio, sizeof(aspect_ratio));
	}

	void camera::view_width(f32 width) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::view_width, &width, sizeof(width));
	}

	void camera::view_height(f32 height) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::view_height, &height, sizeof(height));
	}

	void camera::range(f32 near_z, f32 far_z) const
	{
		assert(is_valid());
		gfx.camera.set_parameter(_id, camera_parameter::near_z, &near_z, sizeof(near_z));
		gfx.camera.set_parameter(_id, camera_parameter::far_z, &far_z, sizeof(far_z));
	}

	math::m4x4 camera::view() const
	{
		assert(is_valid());
		math::m4x4 matrix;
		gfx.camera.get_parameter(_id, camera_parameter::view, &matrix, sizeof(matrix));
		return matrix;
	}

	math::m4x4 camera::projection() const
	{
		assert(is_valid());
		math::m4x4 matrix;
		gfx.camera.get_parameter(_id, camera_parameter::projection, &matrix, sizeof(matrix));
		return matrix;
	}

	math::m4x4 camera::inverse_projection() const
	{
		assert(is_valid());
		math::m4x4 matrix;
		gfx.camera.get_parameter(_id, camera_parameter::inverse_projection, &matrix, sizeof(matrix));
		return matrix;
	}

	math::m4x4 camera::view_projection() const
	{
		assert(is_valid());
		math::m4x4 matrix;
		gfx.camera.get_parameter(_id, camera_parameter::view_projection, &matrix, sizeof(matrix));
		return matrix;
	}

	math::m4x4 camera::inverse_view_projection() const
	{
		assert(is_valid());
		math::m4x4 matrix;
		gfx.camera.get_parameter(_id, camera_parameter::inverse_view_projection, &matrix, sizeof(matrix));
		return matrix;
	}

	math::v3 camera::up() const
	{
		assert(is_valid());
		math::v3 up_vector;
		gfx.camera.get_parameter(_id, camera_parameter::up_vector, &up_vector, sizeof(up_vector));
		return up_vector;
	}

	f32 camera::near_z() const
	{
		assert(is_valid());
		f32 near_z;
		gfx.camera.get_parameter(_id, camera_parameter::near_z, &near_z, sizeof(near_z));
		return near_z;
	}

	f32 camera::far_z() const
	{
		assert(is_valid());
		f32 far_z;
		gfx.camera.get_parameter(_id, camera_parameter::far_z, &far_z, sizeof(far_z));
		return far_z;
	}

	f32 camera::field_of_view() const
	{
		assert(is_valid());
		f32 field_of_view;
		gfx.camera.get_parameter(_id, camera_parameter::field_of_view, &field_of_view, sizeof(field_of_view));
		return field_of_view;
	}

	f32 camera::aspect_ratio() const
	{
		assert(is_valid());
		f32 aspect_ratio;
		gfx.camera.get_parameter(_id, camera_parameter::aspect_ratio, &aspect_ratio, sizeof(aspect_ratio));
		return aspect_ratio;
	}

	f32 camera::view_width() const
	{
		assert(is_valid());
		f32 view_width;
		gfx.camera.get_parameter(_id, camera_parameter::view_width, &view_width, sizeof(view_width));
		return view_width;
	}

	f32 camera::view_height() const
	{
		assert(is_valid());
		f32 view_height;
		gfx.camera.get_parameter(_id, camera_parameter::view_height, &view_height, sizeof(view_height));
		return view_height;
	}

	camera::type camera::projection_type() const
	{
		assert(is_valid());
		camera::type type;
		gfx.camera.get_parameter(_id, camera_parameter::type, &type, sizeof(type));
		return type;
	}

	id::id_type camera::entity_id() const
	{
		assert(is_valid());
		id::id_type entity_id;
		gfx.camera.get_parameter(_id, camera_parameter::entity_id, &entity_id, sizeof(entity_id));
		return entity_id;
	}

	id::id_type add_submesh(const u8 *& data)
	{
		assert(gfx.resources.add_submesh);
		if (!gfx.resources.add_submesh) return id::invalid_id;
		return gfx.resources.add_submesh(data);
	}

	void remove_submesh(id::id_type id)
	{
		if (gfx.resources.remove_submesh) gfx.resources.remove_submesh(id);
	}

	id::id_type add_texture(const u8 *const data)
	{
		assert(gfx.resources.add_texture);
		if (!gfx.resources.add_texture) return id::invalid_id;
		return gfx.resources.add_texture(data);
	}

	void remove_texture(id::id_type id)
	{
		if (gfx.resources.remove_texture) gfx.resources.remove_texture(id);
	}

	id::id_type add_material(material_init_info info)
	{
		assert(gfx.resources.add_material);
		if (!gfx.resources.add_material) return id::invalid_id;
		return gfx.resources.add_material(info);
	}

	void remove_material(id::id_type id)
	{
		if (gfx.resources.remove_material) gfx.resources.remove_material(id);
	}

	id::id_type add_render_item(id::id_type entity_id, id::id_type geometry_content_id,
		u32 material_count, const id::id_type *const material_ids)
	{
		assert(gfx.resources.add_render_item);
		if (!gfx.resources.add_render_item) return id::invalid_id;
		return gfx.resources.add_render_item(entity_id, geometry_content_id, material_count, material_ids);
	}

	void remove_render_item(id::id_type id)
	{
		if (gfx.resources.remove_render_item) gfx.resources.remove_render_item(id);
	}

	// === RHI 路径入口实现（新）===
	// 与 platform_interface 路径并存：UI 层通过 DeviceDesc.platform 决策，
	// 引擎通过 RHIDeviceFactory 被动映射。详见 RHIDeviceFactory.h。
	bool initialize_with_device(const rhi::DeviceDesc& desc)
	{
		if (g_rhiDevice) return false;
		g_rhiDevice = rhi::CreateRHIDevice(desc);
		return g_rhiDevice != nullptr;
	}

	void shutdown_rhi()
	{
		if (!g_rhiDevice) return;
		rhi::DestroyRHIDevice(g_rhiDevice);
		g_rhiDevice = nullptr;
	}

	bool is_rhi_initialized()
	{
		return g_rhiDevice != nullptr;
	}

	rhi::RHIDeviceBase* get_rhi_device()
	{
		return g_rhiDevice;
	}

	bool bind_rhi_device_to_legacy()
	{
		if (!g_rhiDevice) return false;

		// Phase 4b: 按 RHI platform 分发。Metal 注入 metal::core;Vulkan/Dawn 走 RHI 直通,
		// 不依赖 legacy core(legacy Engine/Graphics/Vulkan/ 是 dormant dead code)。
		const auto platform = g_rhiDevice->GetPlatform();

#if defined(__APPLE__)
		if (platform == rhi::RHIPlatform::Metal) {
			auto* metalDevice = dynamic_cast<rhi::MetalDevice*>(g_rhiDevice);
			if (!metalDevice) return false;

			MTL::Device* native = metalDevice->GetNativeDevice();
			if (!native) return false;

			metal::core::set_external_device(native);
			return true;
		}
#endif
		if (platform == rhi::RHIPlatform::Vulkan || platform == rhi::RHIPlatform::Dawn) {
			// RHI 直通路径 — 所有渲染走 rhi::*Device,不经 legacy core。
			return true;
		}

		return false;
	}

}