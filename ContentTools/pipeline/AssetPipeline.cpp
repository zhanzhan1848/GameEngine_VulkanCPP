#include "AssetPipeline.h"

#include "SceneBlobWriter.h"
#include "common/SDF.h"

#include <algorithm>
#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <utility>

namespace primal::tools::pipeline {
namespace {

// Pipeline-level SIGABRT safety net. Third-party libraries (PMP uniform_remeshing,
// xatlas normalize) can trigger C assert() → abort() on degenerate geometry.
// These are NOT catchable C++ exceptions. The handler longjmps back to skip
// ALL in-place modules for the current mesh, preserving the pre-module IR state.
thread_local std::jmp_buf pipeline_jmp_buf;
thread_local bool pipeline_handler_active = false;
void pipeline_sigabrt_handler(int sig) {
    (void)sig;
    if (pipeline_handler_active) {
        longjmp(pipeline_jmp_buf, 1);
    }
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

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
        // Validate index range before SDF: upstream modules (repair/remesh)
        // can leave indices referencing vertices beyond positions.size()
        // after compaction. Clamp/drop bad triangles to avoid SIGBUS in SDF.
        {
            u32 w = 0;
            for (u32 r = 0; r < (u32)bridge.indices.size(); r += 3) {
                if (bridge.indices[r]     >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 1] >= bridge.vertices.size()) continue;
                if (bridge.indices[r + 2] >= bridge.vertices.size()) continue;
                bridge.indices[w++] = bridge.indices[r];
                bridge.indices[w++] = bridge.indices[r + 1];
                bridge.indices[w++] = bridge.indices[r + 2];
            }
            bridge.indices.resize(w);
        }
        generate_sdf(bridge, cfg.sdf_params.resolution);
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
        // Snapshot the IR before in-place modules so we can restore on abort.
        // Some third-party operations (PMP remesh, xatlas) trigger C assert()
        // → abort() on degenerate geometry. The SIGABRT handler longjmps back
        // here; we restore the snapshot and skip modules for this mesh.
        ProcessableMesh snapshot = m;

        pipeline_handler_active = true;
        auto* prev_handler = std::signal(SIGABRT, pipeline_sigabrt_handler);
        bool aborted = (setjmp(pipeline_jmp_buf) != 0);

        if (!aborted) {
            run_in_place_modules(m, cfg, out.warnings);
        }

        if (aborted) {
            // Restore pre-module state and emit a warning.
            m = std::move(snapshot);
            out.warnings.emplace_back(ErrorReport{
                Severity::Warning, "pipeline.module_aborted",
                "pipeline: third-party module aborted (SIGABRT) on this mesh; "
                "skipping all in-place modules, preserving pre-module geometry",
                "pipeline"});
        }

        pipeline_handler_active = false;
        std::signal(SIGABRT, prev_handler);

        if (!aborted && cfg.enable_collision) {
            auto hulls = collision::Run(m, cfg.collision_params, out.warnings);
            for (auto& h : hulls) out.hulls.emplace_back(std::move(h));
        }
    }

    // ---- Phase 2: per-mesh LOD generation ------------------------------
    // If the source asset already has LOD levels (lods.size() > 1), preserve
    // them as-is — don't auto-generate additional LODs. Otherwise, generate
    // LOD chains per-mesh: only meshes with vertex_count >= min_vertices_for_lod
    // get simplified; small meshes are left at LOD 0 only.
    const bool source_has_lod = out.scene.lods.size() > 1;

    struct LodEntry { ProcessableMesh mesh; f32 threshold; };
    struct MeshLodChain {
        utl::vector<LodEntry> levels;  // [0] = LOD 0 (original), [1..N] = lower
    };
    utl::vector<MeshLodChain> mesh_lod_chains;

    const u32 num_lod0_meshes = (u32)lod0.meshes.size();
    mesh_lod_chains.resize(num_lod0_meshes);

    const f32 base_threshold = lod0.screen_threshold > 0.f
        ? lod0.screen_threshold : 0.5f;
    if (lod0.screen_threshold <= 0.f) lod0.screen_threshold = base_threshold;

    if (source_has_lod) {
        // Source asset already has LOD: distribute existing levels into chains.
        // Each LOD level's meshes are paired by index with LOD 0's meshes.
        for (u32 mi = 0; mi < num_lod0_meshes; ++mi) {
            mesh_lod_chains[mi].levels.push_back({lod0.meshes[mi], base_threshold});
            for (u32 li = 1; li < (u32)out.scene.lods.size(); ++li) {
                auto& lod_level = out.scene.lods[li];
                if (mi < (u32)lod_level.meshes.size()) {
                    mesh_lod_chains[mi].levels.push_back(
                        {lod_level.meshes[mi], lod_level.screen_threshold});
                }
            }
        }
        out.warnings.emplace_back(ErrorReport{
            Severity::Info, "pipeline.lod_preserved",
            "pipeline: source asset has " + std::to_string(out.scene.lods.size()) +
            " LOD levels; preserving existing LOD structure (no auto-generation)",
            "pipeline"});
    } else {
        // No existing LOD: auto-generate per mesh.
        for (u32 mi = 0; mi < num_lod0_meshes; ++mi) {
            auto& m = lod0.meshes[mi];
            mesh_lod_chains[mi].levels.push_back({m, base_threshold});

            if (!cfg.enable_lod) continue;

            // Vertex-count gate: skip LOD for meshes below the threshold.
            if ((u32)m.positions.size() < cfg.min_vertices_for_lod) continue;

            utl::vector<ErrorReport> lod_reports;
            utl::vector<ProcessableMesh> lower_lods = lod::Run(m, cfg.lod_params, lod_reports);
            for (auto& r : lod_reports) out.warnings.emplace_back(std::move(r));

            f32 threshold = base_threshold;
            for (u32 i = 0; i < (u32)lower_lods.size(); ++i) {
                threshold /= (cfg.lod_params.ratio > 0.f ? cfg.lod_params.ratio : 0.5f);
                mesh_lod_chains[mi].levels.push_back({std::move(lower_lods[i]), threshold});
            }
        }
    }

    // ---- Phase 3: build packed meshes from per-mesh LOD chains ---------
    // Flatten mesh_lod_chains → PackedMesh entries. Each mesh's LOD levels
    // get lod_id = 0 (original), 1, 2, ... with their respective thresholds.
    // Meshes that didn't generate LODs only emit lod_id=0.
    for (u32 mi = 0; mi < num_lod0_meshes; ++mi) {
        auto& chain = mesh_lod_chains[mi];
        for (u32 li = 0; li < (u32)chain.levels.size(); ++li) {
            const u32 lod_id = (li == 0) ? u32_invalid_id : li;
            build_packed_for_mesh(chain.levels[li].mesh, lod_id,
                                  chain.levels[li].threshold, cfg, out);
        }
    }
}

}  // namespace primal::tools::pipeline
