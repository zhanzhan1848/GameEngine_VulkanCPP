#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::nanite {

// Loads per-mesh offline SDF data from a pipeline model file, merges all
// mesh-level SDFs into a single high-resolution global SDF volume (384³),
// and uploads it as an R16F Texture3D for SDF visualization and GI tracing.
//
// Two encodings exist for the per-mesh u16 data (ContentTools/common/SDF.cpp):
//   - signed (Sponza_pipeline_signed_v5+): fast-winding-number inside bake,
//     packed as pack_float<16>(signedDist + max_dim, 0, 2*max_dim) —
//     negative inside, positive outside. The merge min()s signed distances,
//     but negatives deeper than 8 global voxels flip positive (hollow-
//     architecture sign repair — see SampleMeshSDF): building-scale shells
//     geometrically enclose room air, and an unclamped min() poisons every
//     cavity probe. The result is a signed Mesh DF with a thin negative
//     skin under every surface: relaxed tracers can no longer overstep
//     thin walls (overshoot lands negative and still registers a hit)
//     while open-air probes sample positive distances.
//   - unsigned (legacy Sponza_pipeline.model, pre 2026-08-14 bakes):
//     packed as pack_float<16>(dist, 0, max_dim). Merge takes the min
//     positive distance.
// The disk cache records the encoding in its header (PGSDFC02) so a stale
// cache can never feed the wrong field semantics to the tracers.
class OfflineSDFMerger {
public:
    static constexpr u32 GLOBAL_RES = 384;

    struct MeshSDF {
        u32  resolution[3];
        f32  bounds_min[3];
        f32  bounds_max[3];
        f32  max_dim;               // scale factor used during baking
        utl::vector<u16> data;      // distance, packed as u16 (encoding per signed_encoding_)
        utl::vector<u8>  voxels;    // 255 = near surface / inside
    };

    OfflineSDFMerger() = default;
    ~OfflineSDFMerger();

    // Parse the pipeline model file and extract all LOD0 mesh SDF data.
    // signed_encoding selects the u16 decode (see class comment). Returns
    // the number of meshes with SDF data loaded.
    u32 LoadFromPipelineModel(const char* filePath, bool signed_encoding = true);

    // Encoding this merger instance decodes/bakes with (set by LoadFromPipelineModel).
    bool IsSignedEncoding() const { return signed_encoding_; }

    // Merge all per-mesh SDFs into a single 384³ global volume and upload
    // to GPU as an R16F Texture3D. Must be called after LoadFromPipelineModel.
    // When cachePath is non-null the merged volume is written there so later
    // runs can skip the multi-minute merge via LoadFromCache.
    bool BuildAndUpload(rhi::RHIDeviceBase* device, const char* cachePath = nullptr);

    // Load a previously cached merged volume (written by BuildAndUpload) and
    // upload it — no pipeline model parse, no merge. Returns false when the
    // cache is missing/stale (caller should fall back to the slow path).
    bool LoadFromCache(rhi::RHIDeviceBase* device, const char* cachePath);

    // --- Accessors (valid after BuildAndUpload) ---
    rhi::ResourceHandle GetTexture() const { return global_sdf_texture_; }
    const math::v3&     GetOrigin()  const { return origin_; }
    const math::v3&     GetExtent()  const { return extent_; }
    f32                  GetVoxelSize() const { return voxel_size_; }
    u32                  GetResolution() const { return GLOBAL_RES; }
    bool                 IsValid() const { return global_sdf_texture_ != rhi::handles::INVALID_RESOURCE; }

private:
    utl::vector<MeshSDF> mesh_sdfs_;
    bool signed_encoding_{true};
    math::v3 origin_{0, 0, 0};
    math::v3 extent_{0, 0, 0};
    f32      voxel_size_{0};
    rhi::ResourceHandle global_sdf_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::RHIDeviceBase* device_{nullptr};

    // Trilinear sample of a mesh SDF at a local-space position.
    // Returns signed distance (negative inside, positive outside).
    f32 SampleMeshSDF(const MeshSDF& sdf, f32 lx, f32 ly, f32 lz) const;

    // Shared upload path for merged volumes (BuildAndUpload/LoadFromCache).
    // data layout: z-major res³ u16 half-floats, matching the staging copy.
    bool UploadGlobal(rhi::RHIDeviceBase* device, const u16* data);
};

} // namespace primal::graphics::nanite
