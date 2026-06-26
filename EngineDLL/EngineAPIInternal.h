// Internal header for sharing EngineDLL state between translation units.
// EngineAPI.cpp owns the surfaces vector; other API files access it via these accessors.
#pragma once

#include "CommonHeaders.h"

namespace primal::graphics { struct render_surface; }
namespace primal::graphics { class RenderTexture; }

namespace primal::engine_dll {

// Returns pointer to the render_surface at the given index, or nullptr if out of range.
graphics::render_surface* GetSurface(u32 id);

// Number of registered surfaces.
u32 GetSurfaceCount();

// Offscreen render target registry entry. Lookup via GetRenderTarget(handle).
struct RenderTargetEntry {
    graphics::RenderTexture* texture;
    u32 entity_id;
};

// 1-based handle → entry. nullptr if out-of-range or destroyed.
RenderTargetEntry* GetRenderTarget(u64 handle);

} // namespace primal::engine_dll
