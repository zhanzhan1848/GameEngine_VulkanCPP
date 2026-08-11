#include "Remesh.h"

#include "pmp/surface_mesh.h"
#include "pmp/algorithms/remeshing.h"

#include <igl/qslim.h>
#include <Eigen/Core>
#include <Eigen/Dense>

#include <cmath>
#include <cfloat>
#include <unordered_map>
#include <vector>

namespace primal::tools::remesh {
namespace {

using pmp::SurfaceMesh;
using pmp::Vertex;
using pmp::Point;

// ---- IR ↔ PMP bridge (duplicated from Repair.cpp; Phase 1 keeps per-TU
//      anonymous-namespace isolation per the project convention. If M5 /
//      M6 need the same, factor out into modules/_shared/PmpBridge.h.) ----

struct AttributeChannels {
    pmp::VertexProperty<math::v3>*  normals {nullptr};
    pmp::VertexProperty<math::v4>*  tangents{nullptr};
    pmp::VertexProperty<math::v3>*  colors  {nullptr};
    std::vector<pmp::VertexProperty<math::v2>*> uvs;
};

// Pre-validate triangle list for non-manifold edges before handing it to
// pmp. See Subdivide.cpp / Repair.cpp for rationale.
std::vector<bool> mark_non_manifold_faces(const ProcessableMesh& ir,
                                          u32& dropped_count) {
    const u32 num_tris = (u32)ir.indices.size() / 3;
    std::vector<bool> keep(num_tris, true);
    dropped_count = 0;

    struct EdgeKey {
        u32 a, b;
        bool operator==(const EdgeKey& o) const noexcept { return a == o.a && b == o.b; }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const noexcept {
            return ((size_t)k.a + (size_t)k.b) * ((size_t)k.a + (size_t)k.b + 1) / 2 + k.b;
        }
    };
    std::unordered_map<EdgeKey, u32, EdgeKeyHash> edge_face_count;
    edge_face_count.reserve(static_cast<size_t>(num_tris) * 3);

    for (u32 t = 0; t < num_tris; ++t) {
        const u32 i0 = ir.indices[t * 3 + 0];
        const u32 i1 = ir.indices[t * 3 + 1];
        const u32 i2 = ir.indices[t * 3 + 2];
        if (i0 == i1 || i1 == i2 || i2 == i0) {
            keep[t] = false;
            ++dropped_count;
            continue;
        }
        const EdgeKey e01{i0 < i1 ? i0 : i1, i0 < i1 ? i1 : i0};
        const EdgeKey e12{i1 < i2 ? i1 : i2, i1 < i2 ? i2 : i1};
        const EdgeKey e20{i2 < i0 ? i2 : i0, i2 < i0 ? i0 : i2};
        if (edge_face_count[e01] >= 2 || edge_face_count[e12] >= 2 ||
            edge_face_count[e20] >= 2) {
            keep[t] = false;
            ++dropped_count;
            continue;
        }
        ++edge_face_count[e01];
        ++edge_face_count[e12];
        ++edge_face_count[e20];
    }
    return keep;
}

// Split-only long-edge refinement: repeatedly bisect edges longer than
// `max_edge` at their midpoint until no edge exceeds the threshold. Unlike
// uniform_remeshing, this does NOT collapse short edges — existing detail is
// 100% preserved. New vertices get interpolated attributes. This is the
// correct operation for meshes with giant triangles (walls, floors) where we
// want to break them into smaller pieces without losing any fine detail.
//
// Returns the number of split operations performed.
u32 split_long_edges_only(SurfaceMesh& m, AttributeChannels& attrs,
                          f32 max_edge) {
    if (max_edge <= 0.f) return 0;
    const f32 max_edge_sq = max_edge * max_edge;
    u32 total_splits = 0;

    for (int iteration = 0; iteration < 10; ++iteration) {
        u32 splits_this_pass = 0;
        // Collect long edges first (splitting invalidates the edge iterator).
        struct LongEdge { pmp::Edge e; pmp::Vertex v0; pmp::Vertex v1; };
        std::vector<LongEdge> to_split;
        for (auto e : m.edges()) {
            auto v0 = m.vertex(e, 0);
            auto v1 = m.vertex(e, 1);
            const auto& p0 = m.position(v0);
            const auto& p1 = m.position(v1);
            const f32 dx = p0[0]-p1[0], dy = p0[1]-p1[1], dz = p0[2]-p1[2];
            if (dx*dx + dy*dy + dz*dz > max_edge_sq) {
                to_split.push_back({e, v0, v1});
            }
        }
        if (to_split.empty()) break;

        for (auto& le : to_split) {
            // Edge handle may have been invalidated by prior splits in this
            // pass. Re-check by verifying the two endpoints still share an edge.
            auto h = m.find_halfedge(le.v0, le.v1);
            if (!h.is_valid()) continue;
            auto e = m.edge(h);
            const auto& p0 = m.position(le.v0);
            const auto& p1 = m.position(le.v1);
            // Midpoint
            pmp::Point mid((p0[0]+p1[0])*0.5f, (p0[1]+p1[1])*0.5f, (p0[2]+p1[2])*0.5f);
            auto vnew = m.add_vertex(mid);
            // Interpolate attributes for the new vertex.
            if (attrs.normals) {
                (*attrs.normals)[vnew] = math::v3{
                    ((*attrs.normals)[le.v0].x + (*attrs.normals)[le.v1].x) * 0.5f,
                    ((*attrs.normals)[le.v0].y + (*attrs.normals)[le.v1].y) * 0.5f,
                    ((*attrs.normals)[le.v0].z + (*attrs.normals)[le.v1].z) * 0.5f};
            }
            if (attrs.tangents) {
                const auto& t0 = (*attrs.tangents)[le.v0];
                const auto& t1 = (*attrs.tangents)[le.v1];
                (*attrs.tangents)[vnew] = math::v4{
                    (t0.x+t1.x)*0.5f, (t0.y+t1.y)*0.5f,
                    (t0.z+t1.z)*0.5f, (t0.w+t1.w)*0.5f};
            }
            if (attrs.colors) {
                (*attrs.colors)[vnew] = math::v3{
                    ((*attrs.colors)[le.v0].x + (*attrs.colors)[le.v1].x) * 0.5f,
                    ((*attrs.colors)[le.v0].y + (*attrs.colors)[le.v1].y) * 0.5f,
                    ((*attrs.colors)[le.v0].z + (*attrs.colors)[le.v1].z) * 0.5f};
            }
            for (u32 s = 0; s < (u32)attrs.uvs.size(); ++s) {
                if (attrs.uvs[s]) {
                    const auto& u0 = (*attrs.uvs[s])[le.v0];
                    const auto& u1 = (*attrs.uvs[s])[le.v1];
                    (*attrs.uvs[s])[vnew] = math::v2{
                        (u0.x+u1.x)*0.5f, (u0.y+u1.y)*0.5f};
                }
            }
            m.split(e, vnew);
            ++splits_this_pass;
        }
        total_splits += splits_this_pass;
    }
    return total_splits;
}

SurfaceMesh to_pmp(const ProcessableMesh& ir, AttributeChannels& attrs,
                   const std::vector<bool>& keep_mask) {
    SurfaceMesh m;
    m.reserve(static_cast<unsigned>(ir.positions.size()),
              static_cast<unsigned>(ir.positions.size() * 3),
              static_cast<unsigned>(ir.indices.size() / 3));

    if (!ir.normals.empty()) {
        auto prop = m.add_vertex_property<math::v3>(
            "v:normals", math::v3{0.f, 0.f, 0.f});
        attrs.normals = new pmp::VertexProperty<math::v3>(prop);
    }
    if (!ir.tangents.empty()) {
        auto prop = m.add_vertex_property<math::v4>(
            "v:tangents", math::v4{0.f, 0.f, 0.f, 1.f});
        attrs.tangents = new pmp::VertexProperty<math::v4>(prop);
    }
    if (!ir.colors.empty()) {
        auto prop = m.add_vertex_property<math::v3>(
            "v:colors", math::v3{0.f, 0.f, 0.f});
        attrs.colors = new pmp::VertexProperty<math::v3>(prop);
    }
    attrs.uvs.reserve(ir.uv_sets.size());
    for (u32 i = 0; i < (u32)ir.uv_sets.size(); ++i) {
        if (ir.uv_sets[i].coords.empty()) {
            attrs.uvs.push_back(nullptr);
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "v:uv_%u", i);
        auto prop = m.add_vertex_property<math::v2>(name, math::v2{0.f, 0.f});
        attrs.uvs.push_back(new pmp::VertexProperty<math::v2>(prop));
    }

    for (u32 i = 0; i < (u32)ir.positions.size(); ++i) {
        const math::v3& p = ir.positions[i];
        Vertex v = m.add_vertex(Point(p.x, p.y, p.z));
        if (attrs.normals  && i < (u32)ir.normals.size())  (*attrs.normals )[v] = ir.normals[i];
        if (attrs.tangents && i < (u32)ir.tangents.size()) (*attrs.tangents)[v] = ir.tangents[i];
        if (attrs.colors   && i < (u32)ir.colors.size())   (*attrs.colors  )[v] = ir.colors[i];
        for (u32 s = 0; s < (u32)ir.uv_sets.size(); ++s) {
            if (attrs.uvs[s] && i < (u32)ir.uv_sets[s].coords.size()) {
                (*attrs.uvs[s])[v] = ir.uv_sets[s].coords[i];
            }
        }
    }

    const u32 num_verts = (u32)ir.positions.size();
    const u32 num_tris = (u32)ir.indices.size() / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        if (!keep_mask[t]) continue;
        const u32 i0 = ir.indices[t * 3 + 0];
        const u32 i1 = ir.indices[t * 3 + 1];
        const u32 i2 = ir.indices[t * 3 + 2];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;
        // Defensive bounds check: upstream modules (e.g. repair's
        // garbage_collection) can leave stale indices referencing deleted
        // vertices. Without this guard, Vertex(i0) with i0 >= num_verts
        // triggers properties.h operator[] OOB assert (SIGABRT, uncatchable).
        if (i0 >= num_verts || i1 >= num_verts || i2 >= num_verts) continue;
        m.add_triangle(Vertex(i0), Vertex(i1), Vertex(i2));
    }
    // Remove isolated vertices (vertices with no incident face) that arise
    // when mark_non_manifold_faces drops faces. PMP's uniform_remeshing calls
    // vertex_normals() internally, which iterates every vertex's halfedges —
    // isolated vertices have invalid halfedge connectivity and trigger
    // properties.h operator[] OOB asserts. PMP's garbage_collection does NOT
    // remove isolated vertices (only deleted-flagged ones), so we do it
    // explicitly by deleting + GC.
    for (auto v : m.vertices()) {
        if (m.is_isolated(v)) m.delete_vertex(v);
    }
    m.garbage_collection();
    return m;
}

void from_pmp(const SurfaceMesh& m, const AttributeChannels& attrs,
              ProcessableMesh& ir) {
    ir.positions.clear();
    ir.indices.clear();
    ir.normals.clear();
    ir.tangents.clear();
    ir.colors.clear();
    for (auto& s : ir.uv_sets) s.coords.clear();

    const bool want_normals  = attrs.normals  != nullptr;
    const bool want_tangents = attrs.tangents != nullptr;
    const bool want_colors   = attrs.colors   != nullptr;

    // vmap must be sized to vertices_size() (includes deleted verts) not
    // n_vertices() — see Repair.cpp from_pmp for the same fix.
    std::vector<u32> vmap(m.vertices_size(), u32_invalid_id);
    u32 next_v = 0;
    for (auto v : m.vertices()) {
        vmap[v.idx()] = next_v++;
        Point p = m.position(v);
        ir.positions.emplace_back(math::v3{p[0], p[1], p[2]});
        if (want_normals)  ir.normals.emplace_back((*attrs.normals )[v]);
        if (want_tangents) ir.tangents.emplace_back((*attrs.tangents)[v]);
        if (want_colors)   ir.colors.emplace_back((*attrs.colors  )[v]);
        for (u32 s = 0; s < (u32)attrs.uvs.size(); ++s) {
            if (attrs.uvs[s]) {
                if ((u32)ir.uv_sets.size() <= s) ir.uv_sets.resize(s + 1);
                ir.uv_sets[s].coords.emplace_back((*attrs.uvs[s])[v]);
            }
        }
    }
    for (auto f : m.faces()) {
        unsigned i = 0;
        Vertex fv[3]{};
        for (auto v : m.vertices(f)) {
            if (i < 3) fv[i++] = v;
        }
        if (i != 3) continue;
        ir.indices.emplace_back(vmap[fv[0].idx()]);
        ir.indices.emplace_back(vmap[fv[1].idx()]);
        ir.indices.emplace_back(vmap[fv[2].idx()]);
    }
}

void release_attrs(AttributeChannels& attrs) {
    delete attrs.normals;   attrs.normals = nullptr;
    delete attrs.tangents;  attrs.tangents = nullptr;
    delete attrs.colors;    attrs.colors = nullptr;
    for (auto* p : attrs.uvs) delete p;
    attrs.uvs.clear();
}

// ---- IR ↔ Eigen bridge (libigl qslim path) ------------------------------

// Rebuilds ProcessableMesh from qslim output. Currently preserves positions
// and indices only; normals/tangents/uvs/colors are dropped because libigl
// doesn't natively interpolate them through qslim's birth-index map. A
// warning is pushed for the caller to surface.
void rebuild_from_eigen(ProcessableMesh& io,
                        const Eigen::MatrixXd& U,
                        const Eigen::MatrixXi& G,
                        utl::vector<ErrorReport>& errors) {
    ProcessableMesh out;
    out.name         = io.name;
    out.material_idx = io.material_idx;
    out.positions.reserve(static_cast<u32>(U.rows()));
    for (int r = 0; r < U.rows(); ++r) {
        out.positions.emplace_back(
            math::v3{(f32)U(r, 0), (f32)U(r, 1), (f32)U(r, 2)});
    }
    out.indices.reserve(static_cast<u32>(G.rows() * 3));
    for (int f = 0; f < G.rows(); ++f) {
        out.indices.emplace_back(static_cast<u32>(G(f, 0)));
        out.indices.emplace_back(static_cast<u32>(G(f, 1)));
        out.indices.emplace_back(static_cast<u32>(G(f, 2)));
    }
    if (!io.normals.empty() || !io.tangents.empty() ||
        !io.colors.empty()  || !io.uv_sets.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning,
            "remesh.qslim_dropped_attributes",
            "remesh: qslim decimation dropped per-vertex attributes "
            "(normals/tangents/colors/uvs). Re-derive after remesh.",
            "remesh"});
    }
    io = std::move(out);
}

}  // namespace

// ---- Run -----------------------------------------------------------------

bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors) {
    if (io.positions.empty() || io.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "remesh.empty_input",
            "remesh: empty input mesh, skipping", "remesh"});
        return false;
    }

    if (params.mode == Mode::Decimate) {
        // libigl qslim path.
        const u32 num_faces = (u32)io.indices.size() / 3;
        const u32 target_faces = std::max<u32>(1u, static_cast<u32>(
            std::lround(num_faces * static_cast<double>(params.ratio))));

        Eigen::MatrixXd V(static_cast<Eigen::Index>(io.positions.size()), 3);
        for (u32 i = 0; i < (u32)io.positions.size(); ++i) {
            V(static_cast<Eigen::Index>(i), 0) = io.positions[i].x;
            V(static_cast<Eigen::Index>(i), 1) = io.positions[i].y;
            V(static_cast<Eigen::Index>(i), 2) = io.positions[i].z;
        }
        Eigen::MatrixXi F(static_cast<Eigen::Index>(num_faces), 3);
        for (u32 f = 0; f < num_faces; ++f) {
            F(static_cast<Eigen::Index>(f), 0) = static_cast<int>(io.indices[f * 3 + 0]);
            F(static_cast<Eigen::Index>(f), 1) = static_cast<int>(io.indices[f * 3 + 1]);
            F(static_cast<Eigen::Index>(f), 2) = static_cast<int>(io.indices[f * 3 + 2]);
        }

        Eigen::MatrixXd U;
        Eigen::MatrixXi G;
        Eigen::VectorXi J, I;
        bool ok = false;
        try {
            ok = igl::qslim(V, F, target_faces, U, G, J, I);
        } catch (const std::exception& e) {
            errors.emplace_back(ErrorReport{
                Severity::Error, "remesh.qslim_exception",
                std::string("remesh: qslim threw: ") + e.what(), "remesh"});
            return false;
        } catch (...) {
            errors.emplace_back(ErrorReport{
                Severity::Error, "remesh.qslim_exception",
                "remesh: qslim threw unknown exception", "remesh"});
            return false;
        }
        // qslim returns false in two cases:
        //   (a) input is non-edge-manifold — U/G left empty, real failure.
        //   (b) decimate could not reach target_faces — U/G populated with
        //       partial result, treat as success with a warning.
        if (G.rows() == 0) {
            errors.emplace_back(ErrorReport{
                Severity::Error, "remesh.qslim_failed",
                "remesh: qslim produced no output (likely non-manifold input)",
                "remesh"});
            return false;
        }

        rebuild_from_eigen(io, U, G, errors);
        if (!ok) {
            errors.emplace_back(ErrorReport{
                Severity::Warning, "remesh.qslim_target_not_met",
                "remesh: qslim could not reach target face count " +
                std::to_string(target_faces) + ", got " +
                std::to_string(G.rows()) + " (partial result)", "remesh"});
        } else {
            errors.emplace_back(ErrorReport{
                Severity::Info, "remesh.qslim_ok",
                "remesh: decimated " + std::to_string(num_faces) + " → " +
                std::to_string(G.rows()) + " faces", "remesh"});
        }
        return true;
    }

    // PMP path (Isotropic / Adaptive).
    // Smart-skip: only remesh meshes that have LARGE triangles relative to
    // their bbox. If the longest edge exceeds a fraction of the bbox diagonal,
    // the mesh has "giant triangles" (e.g. a wall covered by 2 huge tris) that
    // hurt meshlet clustering and GPU culling. Meshes with uniformly small
    // triangles (edge << bbox) are already fine — skip to preserve detail.
    const u32 tri_count = (u32)io.indices.size() / 3;
    if (tri_count > 10000) {
        errors.emplace_back(ErrorReport{
            Severity::Info, "remesh.skipped_large",
            "remesh: skipping mesh with " + std::to_string(tri_count) +
            " triangles (>10000; PMP remesh would cause memory blowup)",
            "remesh"});
        return false;
    }

    // Compute bbox diagonal + avg/max edge length in one pass.
    f32 effective_edge_length = params.target_edge_length;
    if (effective_edge_length <= 0.f) {
        math::v3 bmin{FLT_MAX, FLT_MAX, FLT_MAX};
        math::v3 bmax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        double total_edge_len = 0.0;
        u64 edge_count = 0;
        f32 max_edge = 0.f;
        const u32 nidx = (u32)io.indices.size();
        for (u32 t = 0; t + 2 < nidx; t += 3) {
            const u32 i0 = io.indices[t], i1 = io.indices[t+1], i2 = io.indices[t+2];
            if (i0 >= io.positions.size() ||
                i1 >= io.positions.size() ||
                i2 >= io.positions.size()) continue;
            const auto& p0 = io.positions[i0];
            const auto& p1 = io.positions[i1];
            const auto& p2 = io.positions[i2];
            // bbox
            bmin.x = std::min({bmin.x, p0.x, p1.x, p2.x});
            bmin.y = std::min({bmin.y, p0.y, p1.y, p2.y});
            bmin.z = std::min({bmin.z, p0.z, p1.z, p2.z});
            bmax.x = std::max({bmax.x, p0.x, p1.x, p2.x});
            bmax.y = std::max({bmax.y, p0.y, p1.y, p2.y});
            bmax.z = std::max({bmax.z, p0.z, p1.z, p2.z});
            // edges
            auto edge_len = [](const math::v3& a, const math::v3& b) -> f32 {
                const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                return std::sqrt(dx*dx + dy*dy + dz*dz);
            };
            const f32 e01 = edge_len(p0, p1);
            const f32 e12 = edge_len(p1, p2);
            const f32 e20 = edge_len(p2, p0);
            total_edge_len += e01 + e12 + e20;
            edge_count += 3;
            if (e01 > max_edge) max_edge = e01;
            if (e12 > max_edge) max_edge = e12;
            if (e20 > max_edge) max_edge = e20;
        }
        f32 avg_edge = edge_count > 0 ? (f32)(total_edge_len / edge_count) : 0.f;
        const math::v3 d = bmax - bmin;
        const f32 bbox_diag = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (bbox_diag < 1e-6f) return false;  // degenerate

        // Smart-skip: if the longest edge is < 20% of the bbox diagonal, all
        // triangles are small relative to the mesh's extent → already well-
        // tessellated, skip remesh. If max_edge >= 20% of diag, there's at
        // least one giant triangle covering a big chunk of the surface → remesh.
        const f32 edge_to_diag_ratio = max_edge / bbox_diag;
        if (edge_to_diag_ratio < 0.2f) {
            errors.emplace_back(ErrorReport{
                Severity::Info, "remesh.skipped_fine",
                "remesh: skipping fine mesh (max_edge=" + std::to_string(max_edge) +
                " / diag=" + std::to_string(bbox_diag) + " = " +
                std::to_string(edge_to_diag_ratio) + " < 0.2; no giant triangles)",
                "remesh"});
            return false;
        }

        // Split target: use bbox diagonal * fraction, NOT avg_edge.
        // avg_edge can be tiny on meshes with many small triangles — splitting
        // a 30m wall edge down to 0.01m avg_edge takes hundreds of bisections
        // and explodes triangle count (108min runtime). Using diag*0.1 means
        // "no edge longer than 10% of the mesh's bbox diagonal" — bounded and
        // proportional to mesh scale.
        // auto_subdiv controls the fraction: fraction = 0.1 / subdiv.
        //   subdiv=1 → target = diag * 0.1  (max edge = 10% of mesh size)
        //   subdiv=2 → target = diag * 0.05 (max edge = 5%, finer)
        const u32 subdiv = params.auto_subdiv > 0 ? params.auto_subdiv : 1;
        const f32 fraction = 0.1f / static_cast<f32>(subdiv);
        effective_edge_length = bbox_diag * fraction;
        // Clamp: don't go below the current avg_edge (no point splitting edges
        // that are already shorter than the average).
        if (effective_edge_length < avg_edge) effective_edge_length = avg_edge;
        if (effective_edge_length < 1e-6f || !std::isfinite(effective_edge_length))
            effective_edge_length = 0.05f;  // degenerate fallback

        errors.emplace_back(ErrorReport{
            Severity::Info, "remesh.auto_target",
            "remesh: AUTO target=" + std::to_string(effective_edge_length) +
            " (avg_edge=" + std::to_string(avg_edge) +
            " max_edge=" + std::to_string(max_edge) +
            " diag=" + std::to_string(bbox_diag) +
            " ratio=" + std::to_string(edge_to_diag_ratio) +
            " tris=" + std::to_string(nidx/3) + ")", "remesh"});
    }

    AttributeChannels attrs;
    u32 dropped_non_manifold = 0;
    std::vector<bool> keep_mask = mark_non_manifold_faces(io, dropped_non_manifold);
    if (dropped_non_manifold > 0) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "remesh.dropped_non_manifold",
            "remesh: dropped " + std::to_string(dropped_non_manifold) +
            " non-manifold face(s) before processing (pmp requires 2-manifold input)",
            "remesh"});
    }
    SurfaceMesh m = to_pmp(io, attrs, keep_mask);
    // Use split-only long-edge bisection instead of uniform_remeshing.
    // uniform_remeshing's collapse_short_edges over-collapses small triangles
    // on multi-scale meshes (184 zero_bounds on Sponza). Split-only preserves
    // 100% of existing detail — it only breaks long edges into shorter ones.
    // New vertices get interpolated normals/tangents/UVs/colors.
    try {
        if (params.mode == Mode::Decimate) {
            // Decimate path handled earlier; should not reach here.
        } else {
            // Both Isotropic and Adaptive modes use split-only for AUTO mode.
            // The target_edge_length is the max allowed edge length; edges
            // longer than this get bisected at their midpoint iteratively.
            const u32 splits = split_long_edges_only(m, attrs, effective_edge_length);
            if (splits > 0) {
                errors.emplace_back(ErrorReport{
                    Severity::Info, "remesh.splits",
                    "remesh: split " + std::to_string(splits) +
                    " long edges (target=" + std::to_string(effective_edge_length) + ")",
                    "remesh"});
            }
        }
    } catch (const std::exception& e) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "remesh.pmp_exception",
            std::string("remesh: PMP threw: ") + e.what(), "remesh"});
        release_attrs(attrs);
        return false;
    } catch (...) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "remesh.pmp_exception",
            "remesh: PMP threw unknown exception", "remesh"});
        release_attrs(attrs);
        return false;
    }

    from_pmp(m, attrs, io);
    release_attrs(attrs);
    errors.emplace_back(ErrorReport{
        Severity::Info, "remesh.pmp_ok",
        std::string("remesh: ") +
        (params.mode == Mode::Isotropic ? "isotropic" : "adaptive") +
        " remesh done", "remesh"});
    return true;
}

}  // namespace primal::tools::remesh
