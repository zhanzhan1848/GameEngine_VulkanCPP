// Engine/Graphics/WFC/WFCFaceCorners.h
// WFC Phase C.1 Task 13: Pure helpers for socket derivation.
//   - GetFaceCorners: 4 idealized corners of a cube face (CCW from outside)
//   - QuantizeTo2Bit:  maps normalized height [0,1] to 4 buckets (0..3)
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/MathTypes.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

// Returns the 4 idealized corners (in local space, pre-rotation) of the given face
// on a cube centered at the origin. `extent` is interpreted as the per-axis
// half-extent (= WFCTile::bounds_extents), so the cube spans [-extent, +extent]
// on each axis. Corner ordering per face is CCW from outside (i.e. as seen by
// an observer looking at the face from outside the cube).
inline void GetFaceCorners(math::v3 extent, WFCFace face, math::v3 out[4]) {
    const f32 hx = extent.x;
    const f32 hy = extent.y;
    const f32 hz = extent.z;
    switch (face) {
        case WFCFace::PosX:
            out[0] = math::v3{+hx, -hy, -hz};
            out[1] = math::v3{+hx, +hy, -hz};
            out[2] = math::v3{+hx, +hy, +hz};
            out[3] = math::v3{+hx, -hy, +hz};
            break;
        case WFCFace::NegX:
            out[0] = math::v3{-hx, -hy, +hz};
            out[1] = math::v3{-hx, +hy, +hz};
            out[2] = math::v3{-hx, +hy, -hz};
            out[3] = math::v3{-hx, -hy, -hz};
            break;
        case WFCFace::PosY:
            out[0] = math::v3{-hx, +hy, -hz};
            out[1] = math::v3{-hx, +hy, +hz};
            out[2] = math::v3{+hx, +hy, +hz};
            out[3] = math::v3{+hx, +hy, -hz};
            break;
        case WFCFace::NegY:
            out[0] = math::v3{-hx, -hy, +hz};
            out[1] = math::v3{-hx, -hy, -hz};
            out[2] = math::v3{+hx, -hy, -hz};
            out[3] = math::v3{+hx, -hy, +hz};
            break;
        case WFCFace::PosZ:
            out[0] = math::v3{-hx, -hy, +hz};
            out[1] = math::v3{-hx, +hy, +hz};
            out[2] = math::v3{+hx, +hy, +hz};
            out[3] = math::v3{+hx, -hy, +hz};
            break;
        case WFCFace::NegZ:
            out[0] = math::v3{+hx, -hy, -hz};
            out[1] = math::v3{+hx, +hy, -hz};
            out[2] = math::v3{-hx, +hy, -hz};
            out[3] = math::v3{-hx, -hy, -hz};
            break;
    }
}

// Quantize a normalized height [0, 1] to 2 bits (0..3).
// Boundary semantics: [0, 0.25) -> 0; [0.25, 0.5) -> 1; [0.5, 0.75) -> 2; [0.75, ..] -> 3.
inline u8 QuantizeTo2Bit(f32 normalized) {
    if (normalized < 0.25f) return 0;
    if (normalized < 0.50f) return 1;
    if (normalized < 0.75f) return 2;
    return 3;
}

} // namespace primal::graphics::wfc
