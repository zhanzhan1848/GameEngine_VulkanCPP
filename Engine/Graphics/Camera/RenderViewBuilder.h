#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderView.h"
#include "EngineAPI/Camera.h"

namespace primal::graphics {

// Build a RenderView from a camera_id and viewport. Pulls view/projection
// matrices from the engine camera registry via the public camera class.
// Always honors the passed viewport. If camera_id is invalid, view/projection
// matrices default to identity.
RenderView BuildRenderViewFromCameraId(camera_id id, const rhi::ViewportDesc& viewport);

} // namespace primal::graphics
