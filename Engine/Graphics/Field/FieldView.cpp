#include "Graphics/Field/FieldView.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"

namespace primal::graphics::field {

void FieldView::WriteSDFToVolumeParams(
    const FieldDescriptor* cascades, u32 cascade_count,
    volume::VolumeParams& out)
{
    for (u32 c = 0; c < cascade_count && c < 3; ++c) {
        const auto& desc = cascades[c];
        out.SdfOrigins[c]    = {desc.origin.x, desc.origin.y, desc.origin.z, 0.0f};
        out.SdfVoxelSizes[c] = {desc.GetFloat(FieldAttr::VoxelSize), 0.0f, 0.0f, 0.0f};
        out.SdfExtents[c]    = {desc.extent.x, desc.extent.y, desc.extent.z, 0.0f};
        out.SdfResolutions[c] = desc.GetUInt(FieldAttr::Resolution);
    }
    out.SdfCascadeCount = cascade_count;
}

void FieldView::WriteSDFToDDGI(
    const FieldDescriptor* cascades, u32 cascade_count,
    lumen::DDGIVolumeData& out)
{
    // Same packing as VolumeParams: VoxelSizes only uses x component
    for (u32 c = 0; c < cascade_count && c < 3; ++c) {
        const auto& desc = cascades[c];
        out.SdfOrigins[c]    = {desc.origin.x, desc.origin.y, desc.origin.z, 0.0f};
        out.SdfVoxelSizes[c] = {desc.GetFloat(FieldAttr::VoxelSize), 0.0f, 0.0f, 0.0f};
        out.SdfExtents[c]    = {desc.extent.x, desc.extent.y, desc.extent.z, 0.0f};
        out.SdfResolutions[c] = desc.GetUInt(FieldAttr::Resolution);
    }
    out.SdfCascadeCount = cascade_count;
}

void FieldView::WriteSDFToScreenProbe(
    const FieldDescriptor* cascades, u32 cascade_count,
    lumen::ScreenProbeGlobalData& out)
{
    // Key difference: sdf_voxel_sizes fills xyz (not just x)
    for (u32 c = 0; c < cascade_count && c < 3; ++c) {
        const auto& desc = cascades[c];
        f32 vs = desc.GetFloat(FieldAttr::VoxelSize);
        out.sdf_origins[c]     = {desc.origin.x, desc.origin.y, desc.origin.z, 0.0f};
        out.sdf_voxel_sizes[c] = {vs, vs, vs, 0.0f};
        out.sdf_extents[c]     = {desc.extent.x, desc.extent.y, desc.extent.z, 0.0f};
    }
    // Key difference: sdf_resolutions packed as single v4 (xyz=res, w=count)
    out.sdf_resolutions = {
        cascade_count > 0 ? (float)cascades[0].GetUInt(FieldAttr::Resolution) : 0.0f,
        cascade_count > 1 ? (float)cascades[1].GetUInt(FieldAttr::Resolution) : 0.0f,
        cascade_count > 2 ? (float)cascades[2].GetUInt(FieldAttr::Resolution) : 0.0f,
        (float)cascade_count
    };
}

} // namespace primal::graphics::field
