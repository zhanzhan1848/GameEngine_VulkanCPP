// Engine/Graphics/WFC/WFCSocketOps.h
// WFC Phase C.1 Task 14: 8-bit-per-face socket signature encoder.
//
// Packs 4 corner quartiles (2 bits each) into a single u8 per face:
//   bits[0:1] = corner 0 quartile, bits[2:3] = corner 1, ..., bits[6:7] = corner 3.
//
// Half-extent convention: WFCTile::bounds_extents is a per-axis HALF extent,
// so corners live in [-ext, +ext] (see WFCFaceCorners.h).
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"
#include "../../Utilities/MathTypes.h"

namespace primal::graphics::wfc {

// Compute the 8-bit signature for a single face of `tile` after applying
// the variant's Y-axis rotation (variant * 90 degrees).
//   variant 0 = identity, 1 = +90 deg Y (right-hand), 2 = 180, 3 = -90 (=+270).
// Quartile mapping: corner.y in [-hy, +hy] is normalized to [0,1] and then
// quantized via QuantizeTo2Bit (see WFCFaceCorners.h).
u8 ComputeFaceSignature(const WFCTile& tile, u32 variant, WFCFace face);

// Full 6-face encoding packed in u64 (48 bits used, 16 reserved).
// Implemented in Phase C.1 T15.
SocketEncoding DeriveSocketEncoding(const WFCTile& tile, u32 variant);

// Strict + mirror socket compatibility test (Phase C.1).
// Returns true when sig_a == sig_b OR sig_a == Mirror(sig_b). The mirror
// operation swaps corner pairs across the seam: c0<->c3 and c1<->c2.
// Implemented in Phase C.1 T16.
bool AreSocketsCompatible(u8 sig_a, u8 sig_b, WFCFace face);

} // namespace primal::graphics::wfc
