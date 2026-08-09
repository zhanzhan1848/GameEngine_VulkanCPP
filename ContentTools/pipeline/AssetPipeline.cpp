#include "AssetPipeline.h"

#include "SceneBlobWriter.h"
#include "common/SDF.h"

#include <utility>

namespace primal::tools::pipeline {
namespace {

// Run per-mesh in-place modules on a single ProcessableMesh. Each module is
// gated by its enable flag and emits warnings/info into `out` (errors are
// also collected but do not halt the pipeline — caller decides policy).
//
// derive_attributes is special-cased: it has two trigger paths.
//   (a) auto-cascade after subdivide (auto_derive_after_subdivide=true)
//       fires whenever subdivide ran, regardless of enable_derive;
//   (b) standalone (enable_derive=true) fires independently of subdivide.
// Both paths share cfg.derive_params. If both paths fire on the same mesh
// (subdivide on + auto-cascade on + standalone on), derive runs twice —
// the second run is idempotent for non-Faceted normal modes and just
// recomputes the same tangents for MikkTSpace. We accept the redundant
// run rather than tracking "did subdivide actually mutate" state, which
// would couple run_in_place_modules to subdivide's internals.
void run_in_place_modules(ProcessableMesh& m,
                          const Config& cfg,
                          utl::vector<ErrorReport>& reports) {
    if (cfg.enable_repair)    repair::Run(m, cfg.repair_params, reports);
    if (cfg.enable_remesh)    remesh::Run(m, cfg.remesh_params, reports);
    if (cfg.enable_subdivide) subdivide::Run(m, cfg.subdivide_params, reports);
    if (cfg.enable_subdivide && cfg.auto_derive_after_subdivide) {
        derive::Run(m, cfg.derive_params, reports);
    }
    if (cfg.enable_derive)    derive::Run(m, cfg.derive_params, reports);
    if (cfg.enable_uvatlas)   uvatlas::Run(m, cfg.uvatlas_params, reports);
}

// Build a single PackedMesh from ProcessableMesh + (optional) meshlet data.
// Always succeeds — empty meshlet data leaves PackedMesh.meshlets empty and
// the blob writer emits size=0 for the meshlet section, which downstream
// readers tolerate.
void build_packed_for_mesh(const ProcessableMesh& m,
                           u32 lod_id, f32 lod_threshold,
                           const Config& cfg,
                           Result& out) {
    MeshletData md;
    if (cfg.enable_meshlet) {
        meshlet::Run(m, cfg.meshlet_params, md, out.warnings);
    }

    PackedMesh pm = BuildPackedMesh(m, md, lod_id, lod_threshold, out.warnings);

    if (cfg.enable_sdf && !m.positions.empty()) {
        // SDF generation requires the legacy mesh struct because SDF.cpp
        // reads mesh.vertices (AoS). Build a minimal bridge mesh.
        mesh bridge{};
        bridge.vertices.resize(m.positions.size());
        for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
            bridge.vertices[i].position = m.positions[i];
            if (i < m.normals.size())  bridge.vertices[i].normal  = m.normals[i];
            if (i < m.tangents.size()) bridge.vertices[i].tangent = m.tangents[i];
            if (i < m.uv_sets.size() && !m.uv_sets[0].coords.empty())
                bridge.vertices[i].uv = m.uv_sets[0].coords[i];
        }
        bridge.indices = m.indices;
        generate_sdf(bridge);
        pm.sdf = std::move(bridge.sdf);
    }

    out.packed.emplace_back(std::move(pm));
}

}  // namespace

// ---- Run ----------------------------------------------------------------

void Run(ProcessableScene&& in, const Config& cfg, Result& out) {
    out.scene = std::move(in);

    if (out.scene.lods.empty()) {
        out.warnings.emplace_back(ErrorReport{
            Severity::Warning, "pipeline.no_lods",
            "pipeline: scene has no LODs, nothing to do", "pipeline"});
        return;
    }

    // ---- Phase 1: in-place modules on LOD 0 -----------------------------
    // Repair / remesh / subdivide / uvatlas / collision run BEFORE LOD
    // generation so each LOD level derives from cleaned-up geometry.
    ProcessableLod& lod0 = out.scene.lods[0];
    for (auto& m : lod0.meshes) {
        run_in_place_modules(m, cfg, out.warnings);

        if (cfg.enable_collision) {
            auto hulls = collision::Run(m, cfg.collision_params, out.warnings);
            for (auto& h : hulls) out.hulls.emplace_back(std::move(h));
        }
    }

    // ---- Phase 2: LOD generation ----------------------------------------
    // lod::Run on LOD 0's first mesh produces N additional LOD levels.
    // Phase 1 assumes one mesh per LOD (multi-mesh LOD chains are Phase 2).
    if (cfg.enable_lod && lod0.meshes.size() == 1) {
        ProcessableMesh& hi = lod0.meshes[0];
        const f32 base_threshold = lod0.screen_threshold > 0.f
            ? lod0.screen_threshold : 0.5f;
        // Mark LOD 0 with its threshold; subsequent LODs get progressively
        // larger thresholds (halved each level under the default ratio=0.5).
        if (lod0.screen_threshold <= 0.f) lod0.screen_threshold = base_threshold;

        utl::vector<ErrorReport> lod_reports;
        utl::vector<ProcessableMesh> lower_lods = lod::Run(hi, cfg.lod_params, lod_reports);
        for (auto& r : lod_reports) out.warnings.emplace_back(std::move(r));

        f32 threshold = base_threshold;
        for (u32 i = 0; i < (u32)lower_lods.size(); ++i) {
            // Each LOD's switch threshold = previous threshold / ratio.
            // So if ratio=0.5, thresholds go 0.5, 1.0, 2.0, 4.0 ... (in
            // terms of "switch when screen size drops below"). This mirrors
            // how LOD thresholds are interpreted downstream.
            threshold /= (cfg.lod_params.ratio > 0.f ? cfg.lod_params.ratio : 0.5f);
            ProcessableLod new_lod;
            new_lod.screen_threshold = threshold;
            new_lod.meshes.emplace_back(std::move(lower_lods[i]));
            out.scene.lods.emplace_back(std::move(new_lod));
        }
    } else if (cfg.enable_lod) {
        out.warnings.emplace_back(ErrorReport{
            Severity::Warning, "pipeline.lod_multi_mesh_unsupported",
            "pipeline: lod generation with multiple meshes per LOD is Phase 2; skipped",
            "pipeline"});
    }

    // ---- Phase 3: build packed meshes for every LOD level ---------------
    for (u32 lod_idx = 0; lod_idx < (u32)out.scene.lods.size(); ++lod_idx) {
        ProcessableLod& lod = out.scene.lods[lod_idx];
        const u32 lod_id = (lod_idx == 0) ? u32_invalid_id : lod_idx;
        for (auto& m : lod.meshes) {
            build_packed_for_mesh(m, lod_id, lod.screen_threshold, cfg, out);
        }
    }
}

}  // namespace primal::tools::pipeline
