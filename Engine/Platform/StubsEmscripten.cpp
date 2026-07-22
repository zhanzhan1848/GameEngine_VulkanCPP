// Stubs + side-table implementations for content APIs excluded from the
// Emscripten build (ContentToEngine.cpp uses a fake_pointer encoding that
// assumes sizeof(uintptr_t) > sizeof(id::id_type), which fails on wasm32).
#ifdef __EMSCRIPTEN__

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
#include "Engine/Graphics/RHI/Core/RHIGpuMesh.h"
#include "Engine/Graphics/RHI/Core/RHISystem.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"

#include <unordered_map>
#include <memory>
#include <mutex>

namespace primal::content {
namespace {

// Side-tables replacing ContentToEngine.cpp's free_list + pointer-tagging
// scheme. Plain maps keyed by the id register_mesh_asset hands out.
std::mutex& asset_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<id::id_type, graphics::rhi::RHIMeshAsset>& asset_table() {
    static std::unordered_map<id::id_type, graphics::rhi::RHIMeshAsset> t;
    return t;
}
std::unordered_map<id::id_type, std::unique_ptr<graphics::rhi::RHIGpuMesh>>& gpu_mesh_table() {
    static std::unordered_map<id::id_type, std::unique_ptr<graphics::rhi::RHIGpuMesh>> t;
    return t;
}
id::id_type next_mesh_id{1};

} // anonymous namespace

id::id_type register_mesh_asset(graphics::rhi::RHIMeshAsset& asset) {
    std::lock_guard<std::mutex> lock(asset_mutex());
    id::id_type id = next_mesh_id++;
    asset_table()[id] = std::move(asset);
    return id;
}

void foreach_gpu_mesh(GpuMeshCallback callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lock(asset_mutex());
    for (auto& [id, mesh] : gpu_mesh_table()) {
        callback(id, mesh.get());
    }
}

bool get_rhi_mesh_asset(id::id_type id, graphics::rhi::RHIMeshAsset& out) {
    std::lock_guard<std::mutex> lock(asset_mutex());
    auto it = asset_table().find(id);
    if (it == asset_table().end()) return false;
    out = it->second;
    return true;
}

graphics::rhi::RHIGpuMesh* get_rhi_gpu_mesh(id::id_type id) {
    {
        std::lock_guard<std::mutex> lock(asset_mutex());
        auto it = gpu_mesh_table().find(id);
        if (it != gpu_mesh_table().end()) {
            return it->second.get();
        }
    }

    // Lazily create from CPU asset. Mirrors ContentToEngine.cpp:926-955.
    if (graphics::rhi::g_deviceManager.GetDeviceCount() == 0) return nullptr;

    graphics::rhi::RHIMeshAsset asset;
    if (!get_rhi_mesh_asset(id, asset)) return nullptr;

    // nextDeviceId_ starts at 1, so the first registered device is id 1.
    auto* device = graphics::rhi::g_deviceManager.GetDevice(1);
    if (!device) {
        device = graphics::rhi::g_deviceManager.GetDevice(0);
    }
    if (!device) return nullptr;

    auto mesh = std::make_unique<graphics::rhi::RHIGpuMesh>();
    if (!mesh->Initialize(*device, asset)) {
        return nullptr;
    }

    auto* result = mesh.get();
    std::lock_guard<std::mutex> lock(asset_mutex());
    gpu_mesh_table()[id] = std::move(mesh);
    return result;
}

graphics::rhi::ResourceHandle get_rhi_texture_handle(id::id_type) {
    return graphics::rhi::handles::INVALID_RESOURCE;
}

// The metal ContentToEngine encodes RHI id into a fake pointer; on WASM the side-table
// scheme uses the same id for both layers, so get_rhi_mesh_id is the identity function.
id::id_type get_rhi_mesh_id(id::id_type geometry_id) {
    return geometry_id;
}

id::id_type create_resource(const void*, asset_type::type, GraphicsAPI) {
    return id::invalid_id;
}

} // namespace primal::content

// NaniteResourceManager is now compiled for WASM (CMakeLists filter removed);
// AddGeometryRef / ReleaseGeometryRef / GetOrCreateResource have real impls.

#endif // __EMSCRIPTEN__
