// ResourceAPI.cpp - Handle-based wrappers around engine camera/render_item/light_set APIs.
// All handles are u32 (camera) or u64 (render_item, light_set). Invalid = 0 or ~0u.
#include "Common.h"
#include "CommonHeaders.h"
#include "../Graphics/Renderer.h"

#include <cstdio>
#include <atomic>

using namespace primal;

EDITOR_INTERFACE u32 CreateCamera(u32 entity_id, f32 fov, f32 aspect, f32 near_z, f32 far_z)
{
    if (entity_id == id::invalid_id) {
        std::fprintf(stderr, "[CreateCamera] invalid entity_id\n");
        return id::invalid_id;
    }
    if (fov <= 0.f || aspect <= 0.f || near_z <= 0.f || far_z <= near_z) {
        std::fprintf(stderr, "[CreateCamera] invalid params (fov=%.3f aspect=%.3f near=%.3f far=%.3f)\n",
                     fov, aspect, near_z, far_z);
        return id::invalid_id;
    }

    graphics::perspective_camera_init_info info{ entity_id };
    info.field_of_view = fov;
    info.aspect_ratio = aspect;
    info.near_z = near_z;
    info.far_z = far_z;

    graphics::camera cam = graphics::create_camera(info);
    return static_cast<u32>(cam.get_id());
}

EDITOR_INTERFACE void RemoveCamera(u32 cam_id)
{
    if (cam_id == id::invalid_id) return;
    graphics::remove_camera(graphics::camera_id{ cam_id });
}

EDITOR_INTERFACE u64 AddRenderItem(u32 entity_id, u64 geometry_content_id, u32 material_count, const u64* material_ids)
{
    if (entity_id == id::invalid_id) {
        std::fprintf(stderr, "[AddRenderItem] invalid entity_id\n");
        return id::invalid_id;
    }
    if (geometry_content_id == id::invalid_id) {
        std::fprintf(stderr, "[AddRenderItem] invalid geometry_content_id\n");
        return id::invalid_id;
    }
    if (material_count > 0 && !material_ids) {
        std::fprintf(stderr, "[AddRenderItem] material_count>0 but material_ids is null\n");
        return id::invalid_id;
    }

    id::id_type result = graphics::add_render_item(
        entity_id,
        static_cast<id::id_type>(geometry_content_id),
        material_count,
        reinterpret_cast<const id::id_type* const>(material_ids));

    return static_cast<u64>(result);
}

EDITOR_INTERFACE void RemoveRenderItem(u64 render_item_id)
{
    if (render_item_id == id::invalid_id) return;
    graphics::remove_render_item(static_cast<id::id_type>(render_item_id));
}

// --- Light sets ---

namespace {
std::atomic<u64> g_light_set_counter{ 1 };
}

EDITOR_INTERFACE u64 CreateLightSet()
{
    u64 key = g_light_set_counter.fetch_add(1, std::memory_order_relaxed);
    graphics::create_light_set(key);
    return key;
}

EDITOR_INTERFACE void DestroyLightSet(u64 key)
{
    if (key == 0) return;
    graphics::remove_light_set(key);
}
