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
}

namespace primal::graphics::nanite {

NaniteRuntimeResource::~NaniteRuntimeResource() = default;

NaniteRuntimeResource* NaniteResourceManager::GetOrCreateResource(id::id_type) {
    return nullptr;
}

} // namespace primal::graphics::nanite

#endif // __EMSCRIPTEN__
