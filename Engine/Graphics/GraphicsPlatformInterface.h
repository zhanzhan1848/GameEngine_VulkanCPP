#pragma once
#include "CommonHeaders.h"
#include "Renderer.h"

namespace primal::graphics {
	// === Phase 1 Sub-step 1.2.6': platform_interface 已废弃 ===
	// 整套 platform_interface 函数指针表是旧的"后端动态分发"机制，
	// 已被 RHI 抽象（RHIDeviceBase + MetalDevice）+ bind_rhi_device_to_legacy 替代。
	// 新代码不应再持有/传递/读取 platform_interface；旧 Metal/Vulkan/D3D12 后端目录
	// （Metal/MetalInterface.cpp、Vulkan/VulkanInterface.cpp、Direct3D12/D3D12Interface.cpp）
	// 都是这张表的填充者，整体会在 Phase 2 删除。
	// 合法的持有/使用点仅限于：
	//   - Renderer.cpp 旧 forwarding 实现（gfx 静态变量）
	//   - GraphicsPlatform.cpp 旧 set_platform_interface 实现
	//   - 各后端 *Interface.cpp 的 get_platform_interface 实现
	// 这三处都已加 file-scope deprecation suppress。
	struct [[deprecated(
		"platform_interface is deprecated. Use RHI device path: "
		"initialize_with_device + bind_rhi_device_to_legacy + initialize(metal). "
		"See Phase 1 Sub-step 1.2.6' notes."
	)]] platform_interface
	{
		bool(*initialize)(void);
		void(*shutdown)(void);


		struct
		{
			surface(*create)(platform::window);
			void(*remove)(surface_id);
			void(*resize)(surface_id, u32, u32);
			u32(*width)(surface_id);
			u32(*height)(surface_id);
			void(*render)(surface_id, frame_info);
			// Blit `src` (RHI ResourceHandle, which is a u64 alias) into the
			// surface's current drawable and present. Used by Path B
			// (BlitRenderTargetToSurface C ABI). Returns 1 on success, 0 on
			// failure (invalid id / null src / etc.). Declared as u64 (not
			// rhi::ResourceHandle) to keep this deprecated header decoupled
			// from the RHI layer.
			u32(*blit_and_present)(surface_id, u64);
		} surface;

		struct
		{
			void(*create_light_set)(u64);
			void(*remove_light_set)(u64);
#if defined(_MSC_VER)
			light(*create)(light_init_info);
#elif defined(__clang__)
			light(*create)(light_init_info&);
#endif
			void(*remove)(light_id, u64);
			void(*set_parameter)(light_id, u64, light_parameter::parameter, const void *const, u32);
			void(*get_parameter)(light_id, u64, light_parameter::parameter, void *const, u32);
		} light;

		struct
		{
			camera(*create)(camera_init_info);
			void(*remove)(camera_id);
			void(*set_parameter)(camera_id, camera_parameter::parameter, const void *const, u32);
			void(*get_parameter)(camera_id, camera_parameter::parameter, void *const, u32);
		} camera;

		struct {
			id::id_type(*add_submesh)(const u8*&);
			void (*remove_submesh)(id::id_type);
			id::id_type(*add_texture)(const u8 *const);
			void (*remove_texture)(id::id_type);
			id::id_type(*add_material)(material_init_info);
			void (*remove_material)(id::id_type);
			id::id_type(*add_render_item)(id::id_type, id::id_type, u32, const id::id_type *const);
			void(*remove_render_item)(id::id_type);
		} resources;

		graphics_platform platform = (graphics_platform) - 1;
	};
}