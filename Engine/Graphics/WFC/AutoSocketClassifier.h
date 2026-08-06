// Engine/Graphics/WFC/AutoSocketClassifier.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/MathTypes.h"
#include "../RHI/Core/RHIMeshAsset.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;

// AutoSocketClassifier — doorway-aware socket signature encoder.
//   RayTriangle: backface-culling intersection primitive.
//   ClassifyFace / MirrorFlipU: 8×8 occupancy-grid signatures.
//   ClassifyTile: convenience wrapper that classifies all 6 faces at once.
class AutoSocketClassifier {
public:
    // Per-face signatures for one tile variant. face[f] holds the 64-bit
    // occupancy encoding for WFCFace f (index = static_cast<u32>(face)).
    struct FaceSignatures {
        SocketEncoding face[WFC_FACE_COUNT_3D];
    };

    // Möller–Trumbore ray-triangle intersection with backface cull.
    // `*t` (if non-null) returns the hit parameter in units of |dir| — callers
    // that pass a unit dir get a true distance; callers that pass a scaled dir
    // get t in those same scaled units. max_t uses the same units.
    // Returns true on hit; false on miss / backface / out-of-range.
    static bool RayTriangle(const math::v3& origin, const math::v3& dir,
                            const math::v3& v0, const math::v3& v1, const math::v3& v2,
                            f32 max_t, f32* t);

    // Compute a 64-bit occupancy signature for one face of a tile mesh by
    // casting 64 rays (8×8 grid) from outside the face inward.
    //
    // Operates directly on the RHIMeshAsset — classifier runs at
    // mesh-generation time (before RegisterProceduralMesh uploads to GPU), so
    // callers feed the same asset they just populated via emit_box_geometry
    // etc. Reads positions as f32x3 (12-byte stride) and indices as u32.
    //
    // Each ray hits a mesh triangle → bit=1 (solid); misses → bit=0 (opening).
    // Bit ordering: row-major, bit_index = i + j * 8, where i is the U-axis
    // column 0..7 and j is the V-axis row 0..7.
    //
    // mesh:            tile geometry (positions + indices, pre-registration).
    //                  IMPORTANT: only u32-indexed meshes are supported
    //                  (mesh.index_size == 4). u16-indexed meshes return 0
    //                  (callers must convert to u32 first).
    // face:            which of the 6 cube faces to classify
    // variant_transform: rotation matrix applied to sample frame AND verts
    //                    (callers bake variant * 90° rotations into this)
    static SocketEncoding ClassifyFace(
        const graphics::rhi::RHIMeshAsset& mesh,
        WFCFace face,
        const math::m4x4& variant_transform);

    // Mirror-flip the U axis of a signature (used for opposing-face comparison).
    // Each 8-bit row has its bits reversed: bit 0 ↔ bit 7, bit 1 ↔ bit 6, etc.
    static SocketEncoding MirrorFlipU(SocketEncoding sig);

    // Convenience wrapper: classify all 6 faces of a tile in one call, returning
    // a FaceSignatures struct. Equivalent to calling ClassifyFace once per face
    // with the same mesh + variant_transform. Useful when populating a tile's
    // socket set at registration time.
    static FaceSignatures ClassifyTile(
        const graphics::rhi::RHIMeshAsset& mesh,
        const math::m4x4& variant_transform);
};

} // namespace primal::graphics::wfc
