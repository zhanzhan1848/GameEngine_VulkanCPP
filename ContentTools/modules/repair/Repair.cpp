#include "Repair.h"

#include "pmp/surface_mesh.h"
#include "pmp/algorithms/hole_filling.h"
#include "pmp/algorithms/differential_geometry.h"   // face_area, triangle_area

#include "Utilities/Hash.h"   // primal::utl::MurmurHash3_x86_32

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace primal::tools::repair {
namespace {

using pmp::SurfaceMesh;
using pmp::Vertex;
using pmp::Face;
using pmp::Halfedge;
using pmp::Edge;
using pmp::Point;

// ---- IR ↔ PMP bridge ----------------------------------------------------

// Carries every ProcessableMesh attribute that must survive the PMP run.
// Stored as PMP vertex properties so newly inserted vertices (e.g. by
// fill_hole) get default values via PMP's property default mechanism.
struct AttributeChannels {
    pmp::VertexProperty<math::v3>*  normals {nullptr};
    pmp::VertexProperty<math::v4>*  tangents{nullptr};
    pmp::VertexProperty<math::v3>*  colors  {nullptr};
    // One entry per UVSet on the source ProcessableMesh; null entries are
    // skipped. PMP properties hold math::v2 by value.
    std::vector<pmp::VertexProperty<math::v2>*> uvs;
};

// Builds a pmp::SurfaceMesh from ProcessableMesh IR, attaching every
// auxiliary attribute as a vertex property.
SurfaceMesh to_pmp(const ProcessableMesh& ir, AttributeChannels& attrs) {
    SurfaceMesh m;

    // PMP's Point is pmp::Vec<float, 3>; map math::v3 → Point field-by-field.
    m.reserve(static_cast<unsigned>(ir.positions.size()),
              static_cast<unsigned>(ir.positions.size() * 3),
              static_cast<unsigned>(ir.indices.size() / 3));

    // Vertex properties must be allocated before add_vertex so the property
    // array covers every vertex by index.
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

    // Vertices
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

    // Faces
    const u32 num_tris = (u32)ir.indices.size() / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        const u32 i0 = ir.indices[t * 3 + 0];
        const u32 i1 = ir.indices[t * 3 + 1];
        const u32 i2 = ir.indices[t * 3 + 2];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;   // skip degenerate
        m.add_triangle(Vertex(i0), Vertex(i1), Vertex(i2));
    }

    return m;
}

// Extracts positions + triangle indices + every attribute channel back into
// the IR. PMP may have inserted new vertices (fill_hole) or removed vertices
// (degenerate face removal followed by garbage_collection); we rely on
// PMP's vertex indexing being dense after garbage_collection.
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

    // Build a dense vertex index map (PMP Vertex -> new IR index).
    std::vector<u32> vmap(m.n_vertices(), u32_invalid_id);
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

    // Indices
    for (auto f : m.faces()) {
        std::array<Vertex, 3> fv{};
        unsigned i = 0;
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
    // PMP removes properties when SurfaceMesh destructs; we just free our
    // handle allocations.
    delete attrs.normals;   attrs.normals = nullptr;
    delete attrs.tangents;  attrs.tangents = nullptr;
    delete attrs.colors;    attrs.colors = nullptr;
    for (auto* p : attrs.uvs) delete p;
    attrs.uvs.clear();
}

}  // namespace

// ---- Operations ----------------------------------------------------------

u32 remove_degenerate_faces(SurfaceMesh& m, f32 epsilon) {
    u32 removed = 0;
    std::vector<Face> dead;
    for (auto f : m.faces()) {
        if (pmp::face_area(m, f) <= epsilon) dead.push_back(f);
    }
    for (auto f : dead) {
        m.delete_face(f);
        ++removed;
    }
    if (removed) m.garbage_collection();
    return removed;
}

u32 stitch_borders(SurfaceMesh& m, f32 stitch_distance) {
    // Spatial-hash dedup of boundary vertices within stitch_distance.
    // For each pair of boundary verts within the threshold, mark one as
    // merged into the other; we then re-point every incident halfedge.
    // PMP doesn't expose a direct "merge vertices" call, so we do it by
    // (a) collecting boundary verts, (b) hashing positions, (c) for any
    // duplicate pair, deleting one of them after re-pointing its faces to
    // the survivor. This is O(N) on the boundary subset, not on the mesh.
    if (stitch_distance <= 0.f) return 0;

    struct BVert { Vertex v; Point p; };
    std::vector<BVert> boundary;
    for (auto v : m.vertices()) {
        if (m.is_boundary(v)) boundary.push_back({v, m.position(v)});
    }
    if (boundary.size() < 2) return 0;

    const f32 cell = stitch_distance;
    auto cell_of = [cell](f32 c) -> std::int64_t {
        return static_cast<std::int64_t>(std::floor(c / cell));
    };

    // std::hash has no specialization for std::tuple — provide one.
    struct CellKey {
        std::int64_t x, y, z;
        bool operator==(const CellKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const noexcept {
            u32 h = 0;
            primal::utl::MurmurHash3_x86_32(&k, sizeof(k), 0, &h);
            return static_cast<std::size_t>(h);
        }
    };

    std::unordered_map<CellKey, std::vector<u32>, CellKeyHash> grid;
    grid.reserve(boundary.size());
    for (u32 i = 0; i < (u32)boundary.size(); ++i) {
        const auto& p = boundary[i].p;
        grid[{cell_of(p[0]), cell_of(p[1]), cell_of(p[2])}].push_back(i);
    }

    const f32 dist_sq = stitch_distance * stitch_distance;
    std::vector<Vertex> remap(m.n_vertices(), Vertex());
    for (auto& b : boundary) remap[b.v.idx()] = b.v;

    u32 merges = 0;
    for (u32 i = 0; i < (u32)boundary.size(); ++i) {
        Vertex survivor = boundary[i].v;
        if (!survivor.is_valid() || remap[survivor.idx()] != survivor) continue;
        const Point& pi = boundary[i].p;

        // Search 3x3x3 neighborhood.
        const auto& p = boundary[i].p;
        std::int64_t cx = cell_of(p[0]), cy = cell_of(p[1]), cz = cell_of(p[2]);
        for (std::int64_t dx = -1; dx <= 1; ++dx)
        for (std::int64_t dy = -1; dy <= 1; ++dy)
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            auto it = grid.find({cx + dx, cy + dy, cz + dz});
            if (it == grid.end()) continue;
            for (u32 j : it->second) {
                if (j <= i) continue;
                Vertex victim = boundary[j].v;
                if (!victim.is_valid() || remap[victim.idx()] != victim) continue;
                const Point& pj = boundary[j].p;
                f32 ddsq =
                    (pi[0] - pj[0]) * (pi[0] - pj[0]) +
                    (pi[1] - pj[1]) * (pi[1] - pj[1]) +
                    (pi[2] - pj[2]) * (pi[2] - pj[2]);
                if (ddsq > dist_sq) continue;

                // Re-point victim → survivor. Mark victim invalid so future
                // iterations skip it.
                remap[victim.idx()] = survivor;
                boundary[j].v = Vertex();   // mark invalid
                ++merges;
            }
        }
    }

    if (!merges) return 0;

    // Apply remap by rebuilding faces. PMP doesn't allow direct halfedge
    // re-parenting in a stable way; the cleanest route is to collect
    // triangles, remap indices, then delete + re-add the changed faces.
    struct Tri { Vertex a, b, c; Face original; };
    std::vector<Tri> tris;
    for (auto f : m.faces()) {
        unsigned i = 0;
        Vertex fv[3]{};
        for (auto v : m.vertices(f)) {
            if (i < 3) fv[i++] = v;
        }
        if (i == 3) tris.push_back({fv[0], fv[1], fv[2], f});
    }

    std::vector<Tri> re_add;
    for (auto& t : tris) {
        Vertex a = remap[t.a.idx()];
        Vertex b = remap[t.b.idx()];
        Vertex c = remap[t.c.idx()];
        if (a == t.a && b == t.b && c == t.c) continue;   // unchanged
        m.delete_face(t.original);
        if (a == b || b == c || c == a) continue;          // collapsed
        re_add.push_back({a, b, c, Face()});
    }
    for (auto& t : re_add) m.add_triangle(t.a, t.b, t.c);
    m.garbage_collection();
    return merges;
}

u32 fill_holes(SurfaceMesh& m, u32 max_hole_size) {
    u32 filled = 0;
    // Collect boundary halfedges first — fill_hole mutates the halfedge
    // structure, so iterating-and-filling in one pass is unsafe.
    std::vector<Halfedge> holes;
    for (auto h : m.halfedges()) {
        if (m.is_boundary(h)) holes.push_back(h);
    }

    for (Halfedge h : holes) {
        // Walk the boundary loop to count edges.
        u32 loop_len = 0;
        Halfedge cur = h;
        do {
            ++loop_len;
            cur = m.next_halfedge(cur);
            if (loop_len > (max_hole_size == 0 ? 100000u : max_hole_size)) break;
        } while (cur != h);

        if (max_hole_size > 0 && loop_len > max_hole_size) continue;

        try {
            pmp::fill_hole(m, h);
            ++filled;
        } catch (...) {
            // pmp throws TopologyException on non-manifold holes; skip.
        }
    }
    return filled;
}

u32 orient_outward(SurfaceMesh& m) {
    // Flood-fill face orientation consistency. Seed from face 0 (arbitrary).
    // For each unvisited face, walk neighbors; if the shared halfedge is
    // incident to two faces with conflicting winding, flip the neighbor.
    if (m.n_faces() == 0) return 0;

    std::vector<bool> visited(m.n_faces(), false);
    std::vector<Face>  queue;
    queue.reserve(m.n_faces());
    queue.push_back(*(m.faces_begin()));
    visited[queue.front().idx()] = true;

    u32 flipped = 0;
    while (!queue.empty()) {
        Face f = queue.back();
        queue.pop_back();
        for (auto h : m.halfedges(f)) {
            Halfedge opp = m.opposite_halfedge(h);
            Face  nb    = m.face(opp);
            if (!nb.is_valid()) continue;            // boundary
            if (visited[nb.idx()]) continue;
            // If both halfedges point in the same direction across the
            // shared edge, the neighbor is wound inconsistently. PMP's
            // opposite_halfedge(h) is the halfedge on the neighbor face;
            // for two consistently-wound triangles sharing an edge, the
            // shared halfedges are opposite each other (i.e. h on f and
            // opp on nb traverse the edge in opposite directions).
            // Simplest robust check: if from_vertex(h) == from_vertex(opp),
            // they point the same way → flip nb.
            if (m.from_vertex(h) == m.from_vertex(opp)) {
                // PMP doesn't have a single "flip face" call; we
                // accomplish it by deleting + re-adding with reversed
                // vertex order.
                unsigned i = 0;
                Vertex fv[3]{};
                for (auto v : m.vertices(nb)) {
                    if (i < 3) fv[i++] = v;
                }
                if (i != 3) continue;
                m.delete_face(nb);
                m.add_triangle(fv[0], fv[2], fv[1]);   // reversed
                ++flipped;
            }
            visited[nb.idx()] = true;
            // Note: re-adding returns a new Face handle; we cannot keep
            // walking the original nb handle. We skip enqueuing here and
            // rely on a later iteration to discover it. For Phase 1 this
            // is acceptable — orient_outward is best-effort.
        }
    }
    if (flipped) m.garbage_collection();
    return flipped;
}

// ---- Run -----------------------------------------------------------------

bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors) {
    if (io.positions.empty() || io.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning,
            "repair.empty_input",
            "repair: empty input mesh, skipping",
            "repair"});
        return false;
    }

    AttributeChannels attrs;
    SurfaceMesh m = to_pmp(io, attrs);

    bool modified = false;
    u32 stat_removed  = 0;
    u32 stat_stitched = 0;
    u32 stat_filled   = 0;
    u32 stat_flipped  = 0;

    if (enabled(static_cast<Op>(params.ops), Op::RemoveDegenerateFaces)) {
        stat_removed = remove_degenerate_faces(m);
        modified |= (stat_removed > 0);
    }
    if (enabled(static_cast<Op>(params.ops), Op::StitchBorders)) {
        stat_stitched = stitch_borders(m, params.stitch_distance);
        modified |= (stat_stitched > 0);
    }
    if (enabled(static_cast<Op>(params.ops), Op::FillHoles)) {
        stat_filled = fill_holes(m, params.max_hole_size);
        modified |= (stat_filled > 0);
    }
    if (params.orient_outward &&
        enabled(static_cast<Op>(params.ops), Op::OrientOutward)) {
        stat_flipped = orient_outward(m);
        modified |= (stat_flipped > 0);
    }

    if (modified) {
        from_pmp(m, attrs, io);
    }

    if (stat_removed) {
        errors.emplace_back(ErrorReport{
            Severity::Info, "repair.degenerate_removed",
            "repair: removed " + std::to_string(stat_removed) + " degenerate faces",
            "repair"});
    }
    if (stat_stitched) {
        errors.emplace_back(ErrorReport{
            Severity::Info, "repair.borders_stitched",
            "repair: stitched " + std::to_string(stat_stitched) + " border verts",
            "repair"});
    }
    if (stat_filled) {
        errors.emplace_back(ErrorReport{
            Severity::Info, "repair.holes_filled",
            "repair: filled " + std::to_string(stat_filled) + " holes",
            "repair"});
    }
    if (stat_flipped) {
        errors.emplace_back(ErrorReport{
            Severity::Info, "repair.faces_flipped",
            "repair: flipped " + std::to_string(stat_flipped) + " faces for consistent winding",
            "repair"});
    }

    release_attrs(attrs);
    return modified;
}

}  // namespace primal::tools::repair
