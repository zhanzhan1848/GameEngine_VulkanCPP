// Stubs for functions excluded from the Emscripten build
#ifdef __EMSCRIPTEN__

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"

namespace primal::content {
    static id::id_type next_mesh_id{1};
    id::id_type register_mesh_asset(graphics::rhi::RHIMeshAsset&) {
        return next_mesh_id++;
    }

    void foreach_gpu_mesh(GpuMeshCallback) {}

    // RHIMeshAsset lives in ContentToEngine.cpp, which is excluded from WASM
    // (Metal/Metal.hpp dependency). Call sites in RenderSceneSnapshot.cpp
    // guard on the bool return — false routes them to the unit-cube bounds
    // fallback, which is correct for the WASM (non-meshlet) path.
    bool get_rhi_mesh_asset(id::id_type, graphics::rhi::RHIMeshAsset&) {
        return false;
    }
}

namespace primal::graphics::nanite {

NaniteRuntimeResource::~NaniteRuntimeResource() = default;

NaniteRuntimeResource* NaniteResourceManager::GetOrCreateResource(id::id_type) {
    return nullptr;
}

// Ref-counting calls from Cluster.cpp. No-ops on WASM since the actual
// NaniteResourceManager implementation is excluded from the build.
void NaniteResourceManager::AddGeometryRef(id::id_type) {}
void NaniteResourceManager::ReleaseGeometryRef(id::id_type) {}

} // namespace primal::graphics::nanite

#endif // __EMSCRIPTEN__
