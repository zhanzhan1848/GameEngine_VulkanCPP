#pragma once

// =============================================================================
// AIAssetPipelineParams.h — ai_asset_pipeline_params 的唯一权威定义
// （CLI 规划 §9 风险 #2：禁止 POD 手工镜像扩散。ContentTools.cpp（库实现）、
// ContentTools/main.cpp（CLI 调用方）一律 include 本头；C# 侧顺序镜像在
// PrimalEditor_Avalonia/Native/AIAssetPipelineSettings.cs（static_assert 208
// 锁尺寸）。改动本结构必须同步 C# 镜像并跑 ContentToolsCLI --pipeline 冒烟。
// =============================================================================

#include "PrimitiveTypes.h"

#include <cstddef>

namespace primal::tools { struct scene_data; }

struct ai_asset_pipeline_params
{
    // Legacy geo_settings (FBX/OBJ parser-side). These flow into
    // scene_data.settings BEFORE the importer runs. They are NOT pipeline
    // params — they control how the source asset is parsed. Phase 2 M13
    // moved them onto the same POD so a single ABI carries both stages.
    // Defaults match the legacy CookMesh short-form (178.0f, 1, 1, 0, 0, 0, 1).
    f32                         legacy_smoothing_angle;
    u8                          legacy_calculate_normals;
    u8                          legacy_calculate_tangents;
    u8                          legacy_reverse_handedness;
    u8                          legacy_import_embedded_textures;
    u8                          legacy_import_animations;
    u8                          legacy_coalesce_meshes;
    const char*                 input_path;                       // .fbx or .obj
    primal::tools::scene_data*  out_data;                         // rewritten with serialized blob
    u8                          enable_repair;
    u8                          enable_remesh;
    u8                          enable_subdivide;
    u8                          enable_uvatlas;
    u8                          enable_lod;
    u8                          enable_meshlet;
    u8                          enable_collision;
    u8                          enable_derive;                     // M11: standalone derive (independent of subdivide)
    u8                          auto_derive_after_subdivide;       // M11: default 1; cascade derive after subdivide
    u8                          subdivide_scheme;                  // M11: 0=Loop, 1=CatmullClark
    u8                          derive_normal_mode;                // M11: 0=Faceted, 1=Smooth, 2=AreaWeighted, 3=AngleWeighted
    u8                          derive_tangent_mode;               // M11: 0=None, 1=AreaWeighted, 2=MikkTSpace
    u32                         subdivide_levels;                  // M11: default 1
    f32                         lod_ratio;
    f32                         derive_faceted_angle_deg;          // M11: default 60.0 (only used when derive_normal_mode=Faceted)
    u32                         mesh_count;
    u32                         meshlet_count;
    u32                         validation_errors;
    u32                         validation_warnings;
    // Remesh params (appended at tail to preserve C# mirror superset).
    // target_edge_length: 0 = use remesh::Params default (AUTO from bbox).
    // remesh_iterations: 0 = use default (10).
    f32                         target_edge_length;
    u32                         remesh_iterations;
    // -- M13: full pipeline surface --
    // Remesh (additional)
    u8                          remesh_mode;                       // 0=Isotropic, 1=Adaptive, 2=Decimate
    u32                         remesh_auto_subdiv;                // AUTO mode only; 0 = use default (1)
    f32                         remesh_adaptive_approx_error;      // Adaptive only; 0 = use default (0.001)
    f32                         remesh_ratio;                      // Decimate only; 0 = use default (0.5)
    u8                          remesh_use_projection;             // Isotropic/Adaptive; 0 = use default (true)
    // Repair
    u8                          repair_ops;                        // bitfield of repair::Op; 0 = use default (All)
    f32                         repair_stitch_distance;            // 0 = use default (1e-6)
    u32                         repair_max_hole_size;              // 0 = use default (30); 0 here ALSO means "unlimited" per repair.h, so sentinel overlap; ConfigurePipelineFromPOD substitutes default when caller leaves at 0
    u8                          repair_orient_outward;             // 0 = use default (true)
    // UV Atlas
    u8                          uvatlas_target_purpose;            // 0=Texture, 1=Lightmap, 2=Detail
    u32                         uvatlas_max_chart_count;           // 0 = use default (0 = unlimited)
    f32                         uvatlas_gutter;                    // 0 = use default (4.0 for Lightmap, 2.0 for Texture/Detail)
    u8                          uvatlas_stretch_optimization;      // 0 = use default (true)
    u8                          uvatlas_strict_unique_pack;        // 0 = use default (false; true for Lightmap)
    u32                         uvatlas_resolution_hint;           // 0 = use default (0 = let xatlas estimate)
    f32                         uvatlas_texels_per_unit;           // 0 = use default (0 = estimate from resolution_hint)
    u8                          uvatlas_use_input_uvs_as_hint;     // 0 = use default (true for Texture/Detail, false for Lightmap)
    // LOD
    f32                         lod_target_error;                  // 0 = use default (0.01)
    u32                         lod_max_levels;                    // 0 = use default (6)
    u8                          lod_lock_borders;                  // 0 = use default (false)
    u8                          lod_optimize_vcache;               // 0 = use default (true)
    u32                         min_vertices_for_lod;              // 0 = use default (500)
    // Meshlet
    u32                         meshlet_max_vertices;              // 0 = use default (256)
    u32                         meshlet_max_triangles;             // 0 = use default (128)
    f32                         meshlet_cone_weight;               // 0 = use default (0.5)
    u8                          meshlet_optimize_vcache;           // 0 = use default (true)
    u8                          meshlet_compute_bounds;            // 0 = use default (true)
    // Collision
    u32                         collision_max_hulls;               // 0 = use default (8)
    u32                         collision_voxel_resolution;        // 0 = use default (100000)
    u8                          collision_auto_resolution;         // 0 = use default (true)
    f32                         collision_max_concavity;           // 0 = use default (0.001)
    u32                         collision_max_vertices_per_hull;   // 0 = use default (64)
    u8                          collision_pca;                     // 0 = use default (false)
    u32                         collision_plane_downsampling;      // 0 = use default (4)
    u32                         collision_convexhull_downsampling; // 0 = use default (4)
    // SDF
    u8                          enable_sdf;                        // 0 = use default (false; legacy CookMesh hardcoded true)
    // sdf_grid_resolution: voxel grid cells per axis (grid is res^3).
    // 0 = use Config default (32). Note: generate_sdf() itself clamps to
    // [4, 256]; values outside that range are pinned to the nearest bound.
    // Memory/cost scale cubically (~190MB at 256^3) — caller decides.
    u32                         sdf_grid_resolution;
};

static_assert(sizeof(ai_asset_pipeline_params) == 208,
              "ai_asset_pipeline_params ABI drifted from the C# mirror");
