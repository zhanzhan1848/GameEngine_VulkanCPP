#include "DawnInterface.h"
#include "Graphics/GraphicsPlatformInterface.h"

namespace primal::graphics::dawn
{
    // Phase 0: Stub implementations for platform interface.
    // Full implementations will be provided in later phases as the
    // Dawn RHI backend (surface, light, camera, resources) is built out.

    static bool initialize() { return true; }
    static void shutdown() {}

    static surface create_surface(platform::window) { return {}; }
    static void remove_surface(surface_id) {}
    static void resize_surface(surface_id, u32, u32) {}
    static u32 surface_width(surface_id) { return 0; }
    static u32 surface_height(surface_id) { return 0; }
    static void render_surface(surface_id, frame_info) {}

    static void create_light_set(u64) {}
    static void remove_light_set(u64) {}
    static light create_light(light_init_info&) { return {}; }
    static void remove_light(light_id, u64) {}
    static void set_light_parameter(light_id, u64, light_parameter::parameter, const void* const, u32) {}
    static void get_light_parameter(light_id, u64, light_parameter::parameter, void* const, u32) {}

    static camera create_camera(camera_init_info) { return {}; }
    static void remove_camera(camera_id) {}
    static void set_camera_parameter(camera_id, camera_parameter::parameter, const void* const, u32) {}
    static void get_camera_parameter(camera_id, camera_parameter::parameter, void* const, u32) {}

    static id::id_type add_submesh(const u8*&) { return id::invalid_id; }
    static void remove_submesh(id::id_type) {}
    static id::id_type add_texture(const u8* const) { return id::invalid_id; }
    static void remove_texture(id::id_type) {}
    static id::id_type add_material(material_init_info) { return id::invalid_id; }
    static void remove_material(id::id_type) {}
    static id::id_type add_render_item(id::id_type, id::id_type, u32, const id::id_type* const) { return id::invalid_id; }
    static void remove_render_item(id::id_type) {}

    void get_platform_interface(platform_interface& pi)
    {
        pi.initialize = initialize;
        pi.shutdown = shutdown;

        pi.surface_ops.create = create_surface;
        pi.surface_ops.remove = remove_surface;
        pi.surface_ops.resize = resize_surface;
        pi.surface_ops.width = surface_width;
        pi.surface_ops.height = surface_height;
        pi.surface_ops.render = render_surface;

        pi.light.create_light_set = create_light_set;
        pi.light.remove_light_set = remove_light_set;
        pi.light.create = create_light;
        pi.light.remove = remove_light;
        pi.light.set_parameter = set_light_parameter;
        pi.light.get_parameter = get_light_parameter;

        pi.camera_ops.create = create_camera;
        pi.camera_ops.remove = remove_camera;
        pi.camera_ops.set_parameter = set_camera_parameter;
        pi.camera_ops.get_parameter = get_camera_parameter;

        pi.resources.add_submesh = add_submesh;
        pi.resources.remove_submesh = remove_submesh;
        pi.resources.add_texture = add_texture;
        pi.resources.remove_texture = remove_texture;
        pi.resources.add_material = add_material;
        pi.resources.remove_material = remove_material;
        pi.resources.add_render_item = add_render_item;
        pi.resources.remove_render_item = remove_render_item;

        pi.platform = graphics_platform::dawn;
    }
}
