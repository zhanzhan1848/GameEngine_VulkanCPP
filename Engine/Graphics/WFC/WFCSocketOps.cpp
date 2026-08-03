// Engine/Graphics/WFC/WFCSocketOps.cpp
// WFC Phase C.1 Task 14: 8-bit-per-face socket signature encoder.
#include "WFCSocketOps.h"
#include "WFCFaceCorners.h"
#include <cassert>

namespace primal::graphics::wfc {

// Rotate a corner position about the Y axis by (variant * 90) degrees.
// variant 0 = identity
// variant 1 = -90 deg Y  (x,z) -> (-z, x)
// variant 2 = 180 deg Y  (x,z) -> (-x, -z)
// variant 3 = +90 deg Y  (x,z) -> ( z, -x)
//
// Note: the Phase C.1 plan listed variant 1 as +90 deg, but the formula it
// gave (`{-v.z, v.y, v.x}`) actually rotates by -90 deg under the right-hand
// convention. Either direction produces a valid 4-variant tile set — the
// important property is that variants 0..3 partition the 4-element rotation
// group about Y. The convention chosen here is right-hand +90 deg increments
// starting at variant 0 (identity); variant 1 = +90, variant 3 = -90.
static math::v3 RotateCornerY(math::v3 v, u32 variant) {
    assert(variant < 4 && "variant must be 0..3 (rotation group)");
    switch (variant & 3u) {
        case 0: return v;
        case 1: return math::v3{  v.z, v.y, -v.x };  // +90 deg Y
        case 2: return math::v3{ -v.x, v.y, -v.z };  // 180 deg Y
        case 3: return math::v3{ -v.z, v.y,  v.x };  // -90 deg Y (= +270)
    }
    return v;  // unreachable
}

u8 ComputeFaceSignature(const WFCTile& tile, u32 variant, WFCFace face) {
    math::v3 corners_local[4]{};
    GetFaceCorners(tile.bounds_extents, face, corners_local);

    f32 hy = tile.bounds_extents.y;
    if (hy < 1e-6f) hy = 1.0f;   // guard against div-by-zero on degenerate tiles

    u8 sig = 0;
    for (u32 i = 0; i < 4; ++i) {
        math::v3 cw = RotateCornerY(corners_local[i], variant);
        // Half-extent convention: cw.y is in [-hy, +hy]. Map to [0,1].
        f32 normalized = (cw.y + hy) / (2.0f * hy);
        if (!(normalized > 0.0f)) normalized = 0.0f;   // clamp negative + NaN
        if (normalized > 1.0f)    normalized = 1.0f;
        u8 q = QuantizeTo2Bit(normalized);
        sig |= static_cast<u8>(q << (i * 2));
    }
    return sig;
}

// --- T15/T16 stubs ---

SocketEncoding DeriveSocketEncoding(const WFCTile& tile, u32 variant) {
    (void)tile;
    (void)variant;
    return 0;
}

bool AreSocketsCompatible(u8 sig_a, u8 sig_b, WFCFace face) {
    (void)sig_a;
    (void)sig_b;
    (void)face;
    return false;
}

} // namespace primal::graphics::wfc
