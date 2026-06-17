#include "MetalInterface.h"

#include "MetalCommonHeaders.h"
#include "Graphics/GraphicsPlatformInterface.h"
#include "MetalCore.h"
#include "MetalCamera.h"
#include "MetalContent.h"
#include "MetalLight.h"

// === Phase 1 Sub-step 1.2.6': 本文件实现旧 platform_interface 的 Metal 填充器 ===
// 函数签名 + pi.X = ... 赋值都依赖已废弃的 platform_interface。
// RHI 路径已用 MetalDevice + RHIDeviceFactory 替代此填充逻辑，Phase 2 删除整个文件。
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace primal::graphics::metal
{
    void get_platform_interface(platform_interface& pi)
    {
        pi.initialize = core::initialize;
        pi.shutdown = core::shutdown;

        // Basic
        pi.surface.create = core::create_surface;
        pi.surface.remove = core::remove_surface;
        pi.surface.resize = core::resize_surface;
        pi.surface.width = core::surface_width;
        pi.surface.height = core::surface_height;
        pi.surface.render = core::render_surface;

        // Light
        pi.light.create_light_set = light::create_light_set;
		pi.light.remove_light_set = light::remove_light_set;
		pi.light.create = light::create;
		pi.light.remove = light::remove;
		pi.light.set_parameter = light::set_parameter;
		pi.light.get_parameter = light::get_parameter;

        // Camera
        pi.camera.create = camera::create;
		pi.camera.remove = camera::remove;
		pi.camera.set_parameter = camera::set_parameter;
		pi.camera.get_parameter = camera::get_parameter;

        // Resources
        pi.resources.add_submesh = content::submesh::add;
		pi.resources.remove_submesh = content::submesh::remove;
		pi.resources.add_texture = content::texture::add;
		pi.resources.remove_texture = content::texture::remove;
		pi.resources.add_material = content::material::add;
		pi.resources.remove_material = content::material::remove;
		pi.resources.add_render_item = content::render_item::add;
		pi.resources.remove_render_item = content::render_item::remove;

        pi.platform = graphics_platform::metal;
    }
}