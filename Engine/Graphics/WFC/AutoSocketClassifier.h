// Engine/Graphics/WFC/AutoSocketClassifier.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/MathTypes.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;

// AutoSocketClassifier — doorway-aware socket signature encoder.
// (Tasks 6/7 add ClassifyFace, MirrorFlipU, ClassifyTile. Task 5 ships
// only the ray-triangle primitive that those will lean on.)
class AutoSocketClassifier {
public:
    // Möller–Trumbore ray-triangle intersection with backface cull.
    // Returns true on hit; fills *t with hit distance.
    // max_t caps the ray length (face sample rays are 0.05m).
    static bool RayTriangle(const math::v3& origin, const math::v3& dir,
                            const math::v3& v0, const math::v3& v1, const math::v3& v2,
                            f32 max_t, f32* t);
};

} // namespace primal::graphics::wfc
