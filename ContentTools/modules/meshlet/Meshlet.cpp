#include "Meshlet.h"

#include "meshoptimizer.h"

#include <vector>

namespace primal::tools::meshlet {
namespace {

// meshopt packs triangle bytes aligned to 4 — compute the byte length of
// the last meshlet's triangle block accordingly.
size_t aligned_triangle_end(const meshopt_Meshlet& last) {
    return last.triangle_offset + ((last.triangle_count * 3u + 3u) & ~size_t(3));
}

}  // namespace

// ---- Run -----------------------------------------------------------------

bool Run(const ProcessableMesh& m, const Params& params,
         MeshletData& out,
         utl::vector<ErrorReport>& errors) {
    out.meshlets.clear();
    out.meshlet_vertices.clear();
    out.meshlet_triangles.clear();

    if (m.positions.empty() || m.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "meshlet.empty_input",
            "meshlet: empty input mesh, skipping", "meshlet"});
        return false;
    }
    if (params.max_vertices < 3u || params.max_triangles == 0u) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "meshlet.invalid_caps",
            "meshlet: max_vertices must be >= 3 and max_triangles >= 1",
            "meshlet"});
        return false;
    }

    utl::vector<u32> indices = m.indices;
    if (params.optimize_vcache) {
        utl::vector<u32> opt(indices.size());
        meshopt_optimizeVertexCache(
            opt.data(), indices.data(), indices.size(),
            m.positions.size());
        indices = std::move(opt);
    }

    const size_t max_meshlets = meshopt_buildMeshletsBound(
        indices.size(), params.max_vertices, params.max_triangles);
    if (max_meshlets == 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "meshlet.zero_bound",
            "meshlet: meshopt_buildMeshletsBound returned 0", "meshlet"});
        return false;
    }

    std::vector<meshopt_Meshlet> raw_meshlets(max_meshlets);
    std::vector<u32>             raw_vertices(max_meshlets * params.max_vertices);
    std::vector<u8>              raw_triangles(max_meshlets * params.max_triangles * 3u);

    const size_t meshlet_count = meshopt_buildMeshlets(
        raw_meshlets.data(), raw_vertices.data(), raw_triangles.data(),
        indices.data(), indices.size(),
        reinterpret_cast<const float*>(m.positions.data()),
        m.positions.size(), sizeof(math::v3),
        params.max_vertices, params.max_triangles,
        params.cone_weight);
    if (meshlet_count == 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "meshlet.build_failed",
            "meshlet: meshopt_buildMeshlets produced 0 meshlets",
            "meshlet"});
        return false;
    }

    const meshopt_Meshlet& last   = raw_meshlets[meshlet_count - 1];
    const size_t           v_end  = last.vertex_offset + last.vertex_count;
    const size_t           t_end  = aligned_triangle_end(last);

    out.meshlets.reserve(meshlet_count);
    out.meshlet_vertices.assign(raw_vertices.begin(),
                                raw_vertices.begin() + v_end);
    out.meshlet_triangles.assign(raw_triangles.begin(),
                                 raw_triangles.begin() + t_end);

    for (size_t i = 0; i < meshlet_count; ++i) {
        const meshopt_Meshlet& ml = raw_meshlets[i];
        mesh::meshlet out_ml{};
        out_ml.vertex_offset  = (u32)ml.vertex_offset;
        out_ml.triangle_offset = (u32)ml.triangle_offset;
        out_ml.vertex_count   = ml.vertex_count;
        out_ml.triangle_count = ml.triangle_count;

        if (params.compute_bounds) {
            meshopt_Bounds b = meshopt_computeMeshletBounds(
                raw_vertices.data()  + ml.vertex_offset,
                raw_triangles.data() + ml.triangle_offset,
                ml.triangle_count,
                reinterpret_cast<const float*>(m.positions.data()),
                m.positions.size(), sizeof(math::v3));
            out_ml.cone_apex[0] = b.cone_apex[0];
            out_ml.cone_apex[1] = b.cone_apex[1];
            out_ml.cone_apex[2] = b.cone_apex[2];
            out_ml.cone_axis[0] = b.cone_axis[0];
            out_ml.cone_axis[1] = b.cone_axis[1];
            out_ml.cone_axis[2] = b.cone_axis[2];
            out_ml.cone_cutoff  = b.cone_cutoff;
            out_ml.center[0]    = b.center[0];
            out_ml.center[1]    = b.center[1];
            out_ml.center[2]    = b.center[2];
            out_ml.radius       = b.radius;
        }
        out.meshlets.emplace_back(out_ml);
    }

    errors.emplace_back(ErrorReport{
        Severity::Info, "meshlet.ok",
        "meshlet: built " + std::to_string(meshlet_count) +
        " meshlets (" + std::to_string(v_end) + " vert refs, " +
        std::to_string(t_end) + " triangle bytes)", "meshlet"});
    return true;
}

}  // namespace primal::tools::meshlet
