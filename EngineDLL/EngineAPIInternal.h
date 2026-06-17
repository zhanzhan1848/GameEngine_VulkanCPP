// Internal header for sharing EngineDLL state between translation units.
// EngineAPI.cpp owns the surfaces vector; other API files access it via these accessors.
#pragma once

#include "CommonHeaders.h"

namespace primal::graphics { struct render_surface; }

namespace primal::engine_dll {

// Returns pointer to the render_surface at the given index, or nullptr if out of range.
graphics::render_surface* GetSurface(u32 id);

// Number of registered surfaces.
u32 GetSurfaceCount();

} // namespace primal::engine_dll
