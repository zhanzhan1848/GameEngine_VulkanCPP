// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "CommonHeaders.h"
#include "VulkanInterface.h"
#include "VulkanCore.h"
#include "VulkanCamera.h"
#include "VulkanLight.h"
#include "Graphics/GraphicsPlatformInterface.h"

namespace primal::graphics::vulkan {

    void get_platform_interface(platform_interface& pi)
    {
        pi.initialize = core::initialize;
        pi.shutdown = core::shutdown;

        pi.surface.create = core::create_surface;
        pi.surface.remove = core::remove_surface;
        pi.surface.resize = core::resize_surface;
        pi.surface.width = core::surface_width;
        pi.surface.height = core::surface_height;
        pi.surface.render = core::render_surface;

        pi.light.create_light_set = light::create_light_set;
        pi.light.remove_light_set = light::remove_light_set;
        pi.light.create = light::create;
        pi.light.remove = light::remove;
        pi.light.set_parameter = light::set_parameter;
        pi.light.get_parameter = light::get_parameter;

        pi.camera.create = camera::create;
        pi.camera.remove = camera::remove;
        pi.camera.set_parameter = camera::set_parameter;
        pi.camera.get_parameter = camera::get_parameter;
        /*pi.resources.add_submesh = content::submesh::add;
        pi.resources.remove_submesh = content::submesh::remove;*/

        pi.platform = graphics_platform::vulkan_1;
    }

}