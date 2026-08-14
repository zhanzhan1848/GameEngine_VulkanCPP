#include "Lod.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace primal::tools::lod {
namespace {

// Remove unreferenced vertices from the pool, remap indices, and carry
// every per-vertex attribute (positions/normals/tangents/colors/uv_sets)
// through the same permutation. Order of survivors = first-use order, so
// when called after meshopt_optimizeVertexCache the resulting vertex
// layout has cache-friendly locality (effectively also a vfetch opt).
void compact_vertices(ProcessableMesh& m) {
    const u32 nv = (u32)m.positions.size();
    if (nv == 0) return;

    std::vector<u32> remap(nv, u32_invalid_id);
    u32 next = 0;
    for (const u32 idx : m.indices) {
        if (idx < nv && remap[idx] == u32_invalid_id) {
            remap[idx] = next++;
        }
    }
    if (next == nv) return;  // no compaction possible

    ProcessableMesh out;
    out.name         = m.name;
    out.material_idx = m.material_idx;
    out.positions.reserve(next);
    if (!m.normals.empty())  out.normals.reserve(next);
    if (!m.tangents.empty()) out.tangents.reserve(next);
    if (!m.colors.empty())   out.colors.reserve(next);
    out.uv_sets.resize(m.uv_sets.size());
    for (u32 s = 0; s < (u32)m.uv_sets.size(); ++s) {
        out.uv_sets[s].purpose = m.uv_sets[s].purpose;
        if (!m.uv_sets[s].coords.empty()) out.uv_sets[s].coords.reserve(next);
    }

    for (u32 i = 0; i < nv; ++i) {
        if (remap[i] == u32_invalid_id) continue;
        out.positions.emplace_back(m.positions[i]);
        if (!m.normals.empty())  out.normals.emplace_back(m.normals[i]);
        if (!m.tangents.empty()) out.tangents.emplace_back(m.tangents[i]);
        if (!m.colors.empty())   out.colors.emplace_back(m.colors[i]);
        for (u32 s = 0; s < (u32)m.uv_sets.size(); ++s) {
            if (!m.uv_sets[s].coords.empty()) {
                out.uv_sets[s].coords.emplace_back(m.uv_sets[s].coords[i]);
            }
        }
    }

    out.indices.reserve(m.indices.size());
    for (const u32 idx : m.indices) {
        out.indices.emplace_back(remap[idx]);
    }

    m = std::move(out);
}

}  // namespace

// ---- Run -----------------------------------------------------------------

utl::vector<ProcessableMesh> Run(const ProcessableMesh& hi,
                                 const Params& params,
                                 utl::vector<ErrorReport>& errors) {
    utl::vector<ProcessableMesh> lods;

    if (hi.positions.empty() || hi.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "lod.empty_input",
            "lod: empty input mesh, skipping", "lod"});
        return lods;
    }
    if (params.ratio <= 0.f || params.ratio >= 1.f) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "lod.invalid_ratio",
            "lod: ratio must be in (0,1)", "lod"});
        return lods;
    }
    if (params.max_levels == 0) return lods;

    ProcessableMesh current = hi;   // deep copy; mutated each iteration

    for (u32 level = 1; level <= params.max_levels; ++level) {
        const u32 src_idx_count = (u32)current.indices.size();
        if (src_idx_count < 12) break;   // need ≥ 4 tris to simplify further

        const u32 target_unrounded = static_cast<u32>(
            std::lround(src_idx_count * static_cast<double>(params.ratio)));
        const u32 target = std::max<u32>(3u, (target_unrounded / 3u) * 3u);
        if (target >= src_idx_count) break;

        utl::vector<u32> dst_indices(src_idx_count);
        f32 error = 0.f;
        // meshopt_simplify options:
        //   0x01 — lock border verts (don't move)
        //   0x02 — enable early-out at target_error (default on)
        // We pass through lock_borders only.
        const unsigned options = params.lock_borders ? 0x01u : 0x00u;

        const size_t simplified_count = meshopt_simplify(
            dst_indices.data(),
            current.indices.data(), src_idx_count,
            reinterpret_cast<const float*>(current.positions.data()),
            current.positions.size(), sizeof(math::v3),
            target, params.target_error,
            options, &error);

        if (simplified_count == 0) break;
        if (simplified_count >= src_idx_count) break;  // no progress

        dst_indices.resize(simplified_count);

        ProcessableMesh next = current;   // copy attributes
        next.indices = std::move(dst_indices);

        if (params.optimize_vcache) {
            utl::vector<u32> opt_indices(next.indices.size());
            meshopt_optimizeVertexCache(
                opt_indices.data(),
                next.indices.data(), next.indices.size(),
                next.positions.size());
            next.indices = std::move(opt_indices);
        }

        compact_vertices(next);

        lods.emplace_back(std::move(next));
        current = lods.back();   // deep copy for next iteration's source
    }

    if (lods.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "lod.no_levels_generated",
            "lod: no levels generated (mesh too small or already minimal)",
            "lod"});
    } else {
        errors.emplace_back(ErrorReport{
            Severity::Info, "lod.ok",
            "lod: generated " + std::to_string(lods.size()) +
            " LOD levels", "lod"});
    }
    return lods;
}

}  // namespace primal::tools::lod
