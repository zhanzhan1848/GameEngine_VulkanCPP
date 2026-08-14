#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::nanite {

// Loads per-mesh offline SDF data from a pipeline model file, merges all
// mesh-level SDFs into a single high-resolution global SDF volume (256³),
// and uploads it as an R16F Texture3D for SDF visualization.
//
// The merge converts unsigned per-mesh distances into a signed global field:
//   - For each global voxel, find the nearest mesh SDF that contains it.
//   - Sample the mesh SDF to get unsigned distance.
//   - Use the mesh's occupancy voxels to determine inside/outside.
//   - Inside voxels get negative distance; outside get positive.
//   - Take the minimum signed distance across all meshes.
class OfflineSDFMerger {
public:
    static constexpr u32 GLOBAL_RES = 384;

    struct MeshSDF {
        u32  resolution[3];
        f32  bounds_min[3];
        f32  bounds_max[3];
        f32  max_dim;               // scale factor used during baking
        utl::vector<u16> data;      // unsigned distance, packed as u16
        utl::vector<u8>  voxels;    // 255 = near surface / inside
    };

    OfflineSDFMerger() = default;
    ~OfflineSDFMerger();

    // Parse the pipeline model file and extract all LOD0 mesh SDF data.
    // Returns the number of meshes with SDF data loaded.
    u32 LoadFromPipelineModel(const char* filePath);

    // Merge all per-mesh SDFs into a single 256³ global volume and upload
    // to GPU as an R16F Texture3D. Must be called after LoadFromPipelineModel.
    bool BuildAndUpload(rhi::RHIDeviceBase* device);

    // --- Accessors (valid after BuildAndUpload) ---
    rhi::ResourceHandle GetTexture() const { return global_sdf_texture_; }
    const math::v3&     GetOrigin()  const { return origin_; }
    const math::v3&     GetExtent()  const { return extent_; }
    f32                  GetVoxelSize() const { return voxel_size_; }
    u32                  GetResolution() const { return GLOBAL_RES; }
    bool                 IsValid() const { return global_sdf_texture_ != rhi::handles::INVALID_RESOURCE; }

private:
    utl::vector<MeshSDF> mesh_sdfs_;
    math::v3 origin_{0, 0, 0};
    math::v3 extent_{0, 0, 0};
    f32      voxel_size_{0};
    rhi::ResourceHandle global_sdf_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::RHIDeviceBase* device_{nullptr};

    // Trilinear sample of a mesh SDF at a local-space position.
    // Returns signed distance (negative inside, positive outside).
    f32 SampleMeshSDF(const MeshSDF& sdf, f32 lx, f32 ly, f32 lz) const;
};

} // namespace primal::graphics::nanite
