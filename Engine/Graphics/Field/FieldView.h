#pragma once

#include "CommonHeaders.h"
#include "Graphics/Field/FieldDescriptor.h"

// Forward declarations — consumers include their own headers
namespace primal::graphics::volume { struct VolumeParams; }
namespace primal::graphics::lumen { struct DDGIVolumeData; }
namespace primal::graphics::lumen { struct ScreenProbeGlobalData; }

namespace primal::graphics::field {

struct FieldView {
    // ---- VolumeParams (VolumeTypes.h) ----
    // SdfOrigins[v4 x3], SdfVoxelSizes[v4 x3] (only x used), SdfExtents[v4 x3],
    // SdfResolutions[u32 x3], SdfCascadeCount[u32]
    static void WriteSDFToVolumeParams(
        const FieldDescriptor* cascades, u32 cascade_count,
        volume::VolumeParams& out);

    // ---- DDGIVolumeData (LumenDDGIPass.h) ----
    // Same packing as VolumeParams:
    // SdfOrigins[v4 x3], SdfVoxelSizes[v4 x3] (only x used), SdfExtents[v4 x3],
    // SdfResolutions[u32 x3], SdfCascadeCount[u32]
    static void WriteSDFToDDGI(
        const FieldDescriptor* cascades, u32 cascade_count,
        lumen::DDGIVolumeData& out);

    // ---- ScreenProbeGlobalData (ScreenProbeGIPass.h) ----
    // Key difference: sdf_voxel_sizes = {vs, vs, vs, 0} (xyz all filled)
    // Key difference: sdf_resolutions = v4{res0, res1, res2, cascadeCount} (packed float)
    static void WriteSDFToScreenProbe(
        const FieldDescriptor* cascades, u32 cascade_count,
        lumen::ScreenProbeGlobalData& out);
};

} // namespace primal::graphics::field
