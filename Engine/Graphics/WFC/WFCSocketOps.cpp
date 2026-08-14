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

// --- T16 stub ---

// Phase C.1 Task 15: pack 6 face signatures into a u64.
// Layout: [posX:8][negX:8][posY:8][negY:8][posZ:8][negZ:8][reserved:16]
// Face order matches the WFCFace enum (PosX=0, NegX=1, ..., NegZ=5), so byte f
// of the encoding is the signature for WFCFace f. Top 16 bits are reserved
// for future extensions (e.g. half-tile or sub-face variants) and left at 0.
SocketEncoding DeriveSocketEncoding(const WFCTile& tile, u32 variant) {
    SocketEncoding enc = 0;
    for (u32 f = 0; f < WFC_FACE_COUNT_3D; ++f) {
        WFCFace face = static_cast<WFCFace>(f);
        u8 sig = ComputeFaceSignature(tile, variant, face);
        enc |= static_cast<u64>(sig) << (f * 8);
    }
    return enc;
}

// Mirror a face signature by swapping the two corner pairs that meet at the
// seam when two tiles abut. With quartile layout c0=bits[1:0], c1=bits[3:2],
// c2=bits[5:4], c3=bits[7:6], the mirror swaps c0<->c3 and c1<->c2.
static u8 MirrorSignature(u8 sig) {
    u8 c0 = static_cast<u8>((sig >> 0) & 0x3);
    u8 c1 = static_cast<u8>((sig >> 2) & 0x3);
    u8 c2 = static_cast<u8>((sig >> 4) & 0x3);
    u8 c3 = static_cast<u8>((sig >> 6) & 0x3);
    return static_cast<u8>((c3 << 0) | (c2 << 2) | (c1 << 4) | (c0 << 6));
}

bool AreSocketsCompatible(u8 sig_a, u8 sig_b, WFCFace /*face*/) {
    // Strict match: identical signatures (covers mirror-symmetric faces like a
    // cube side, where mirroring the signature returns the same value).
    if (sig_a == sig_b) return true;
    // Mirror match: sig_a equals the seam-mirror of sig_b.
    if (sig_a == MirrorSignature(sig_b)) return true;
    return false;
}

} // namespace primal::graphics::wfc
