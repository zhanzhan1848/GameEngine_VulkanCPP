#include "Repair.h"

#include "pmp/surface_mesh.h"
#include "pmp/algorithms/hole_filling.h"
#include "pmp/algorithms/differential_geometry.h"   // face_area, triangle_area

#include "Utilities/Hash.h"   // primal::utl::MurmurHash3_x86_32

#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Pre-validate triangle list for non-manifold edges before handing it to
// pmp. Same rationale as Subdivide.cpp: pmp::SurfaceMesh::add_triangle
// throws TopologyException on any edge with 2 incident faces already.
// Repair can't fix non-manifold geometry that pmp refuses to even load,
// so we drop those faces at IR→PMP boundary and let the manifold portion
// flow through to stitch/fill/orient.
//
// Returns a keep/drop mask sized to num_tris. dropped_count gets the total.
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

// Builds a pmp::SurfaceMesh from ProcessableMesh IR, attaching every
// auxiliary attribute as a vertex property.
SurfaceMesh to_pmp(const ProcessableMesh& ir, AttributeChannels& attrs,
                   const std::vector<bool>& keep_mask) {
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
        if (!keep_mask[t]) continue;
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
    // CRITICAL: vmap must be sized to vertices_size() (the property array
    // capacity, INCLUDING deleted verts), NOT n_vertices() (which excludes
    // them). PMP's vertex iterator skips deleted verts but their idx values
    // still occupy the [n_vertices, vertices_size) range — using n_vertices
    // for vmap size causes OOB writes when v.idx() >= n_vertices.
    const size_t vmap_size = m.vertices_size();
    auto get_v3 = [](pmp::VertexProperty<math::v3>* prop, size_t idx) -> math::v3 {
        if (!prop) return {0,0,0};
        auto& vec = prop->vector();
        return (idx < vec.size()) ? vec[idx] : math::v3{0,0,0};
    };
    auto get_v4 = [](pmp::VertexProperty<math::v4>* prop, size_t idx) -> math::v4 {
        if (!prop) return {0,0,0,1};
        auto& vec = prop->vector();
        return (idx < vec.size()) ? vec[idx] : math::v4{0,0,0,1};
    };
    auto get_v2 = [](pmp::VertexProperty<math::v2>* prop, size_t idx) -> math::v2 {
        if (!prop) return {0,0};
        auto& vec = prop->vector();
        return (idx < vec.size()) ? vec[idx] : math::v2{0,0};
    };

    std::vector<u32> vmap(vmap_size, u32_invalid_id);
    u32 next_v = 0;
    for (auto v : m.vertices()) {
        const size_t vi = (size_t)v.idx();
        vmap[vi] = next_v++;
        Point p = m.position(v);
        ir.positions.emplace_back(math::v3{p[0], p[1], p[2]});
        if (want_normals)  ir.normals.emplace_back(get_v3(attrs.normals,  vi));
        if (want_tangents) ir.tangents.emplace_back(get_v4(attrs.tangents, vi));
        if (want_colors)   ir.colors.emplace_back(get_v3(attrs.colors,    vi));
        for (u32 s = 0; s < (u32)attrs.uvs.size(); ++s) {
            if (attrs.uvs[s]) {
                if ((u32)ir.uv_sets.size() <= s) ir.uv_sets.resize(s + 1);
                ir.uv_sets[s].coords.emplace_back(get_v2(attrs.uvs[s], vi));
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

// Compute a vertex remap that merges coincident boundary vertices within
// stitch_distance. Returns an identity-init vector indexed by Vertex::idx()
// where victim entries point to their survivor's idx. Does NOT modify the
// SurfaceMesh topology — the remap is applied later at the IR level (see
// apply_vertex_remap). This avoids the delete_face+add_triangle strategy
// that corrupted PMP's halfedge invariants (properties.h OOB assert → abort).
//
// Returns the merge count (0 = nothing to stitch). out_remap is resized to
// m.vertices_size() (property array capacity) and identity-init even when
// merges == 0. Sized to vertices_size() not n_vertices() because boundary
// vertex idx values occupy [0, vertices_size).
u32 compute_boundary_remap(const SurfaceMesh& m, f32 stitch_distance,
                           std::vector<u32>& out_remap) {
    const size_t vs = m.vertices_size();
    out_remap.resize(vs);
    for (size_t i = 0; i < vs; ++i) out_remap[i] = (u32)i;
    if (stitch_distance <= 0.f) return 0;

    struct BVert { u32 idx; Point p; };
    std::vector<BVert> boundary;
    for (auto v : m.vertices()) {
        if (m.is_boundary(v)) boundary.push_back({(u32)v.idx(), m.position(v)});
    }
    if (boundary.size() < 2) return 0;

    const f32 cell = stitch_distance;
    auto cell_of = [cell](f32 c) -> std::int64_t {
        return static_cast<std::int64_t>(std::floor(c / cell));
    };

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
    std::vector<bool> dead(boundary.size(), false);
    u32 merges = 0;

    for (u32 i = 0; i < (u32)boundary.size(); ++i) {
        if (dead[i]) continue;
        const u32 survivor_idx = boundary[i].idx;
        const Point& pi = boundary[i].p;

        std::int64_t cx = cell_of(pi[0]), cy = cell_of(pi[1]), cz = cell_of(pi[2]);
        for (std::int64_t dx = -1; dx <= 1; ++dx)
        for (std::int64_t dy = -1; dy <= 1; ++dy)
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            auto it = grid.find({cx + dx, cy + dy, cz + dz});
            if (it == grid.end()) continue;
            for (u32 j : it->second) {
                if (j <= i || dead[j]) continue;
                const Point& pj = boundary[j].p;
                f32 ddsq =
                    (pi[0] - pj[0]) * (pi[0] - pj[0]) +
                    (pi[1] - pj[1]) * (pi[1] - pj[1]) +
                    (pi[2] - pj[2]) * (pi[2] - pj[2]);
                if (ddsq > dist_sq) continue;
                // Merge victim j → survivor i at the IR index level.
                out_remap[boundary[j].idx] = survivor_idx;
                dead[j] = true;
                ++merges;
            }
        }
    }
    return merges;
}

// Compute boundary vertex remap directly on the IR (ProcessableMesh), using
// IR dense vertex indices. This is the post-from_pmp stitch: it finds
// coincident boundary vertices (vertices referenced by boundary edges —
// edges used by only one triangle) and merges them. Returns merge count.
u32 compute_ir_boundary_remap(const ProcessableMesh& io, f32 stitch_distance,
                              std::vector<u32>& out_remap) {
    const u32 num_verts = (u32)io.positions.size();
    out_remap.resize(num_verts);
    for (u32 i = 0; i < num_verts; ++i) out_remap[i] = i;
    if (stitch_distance <= 0.f || num_verts == 0) return 0;

    // Find boundary vertices: a vertex is on the boundary if any of its
    // incident edges is used by only one triangle (open edge).
    // Build edge → face count.
    struct EdgeKey { u32 a, b; bool operator==(const EdgeKey& o) const { return a==o.a && b==o.b; } };
    struct EdgeHash { size_t operator()(const EdgeKey& k) const noexcept {
        return ((size_t)k.a * 73856093) ^ ((size_t)k.b * 19349663);
    }};
    std::unordered_map<EdgeKey, u32, EdgeHash> edge_count;
    edge_count.reserve(io.indices.size());
    const u32 num_tris = (u32)io.indices.size() / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        u32 i0 = io.indices[t*3], i1 = io.indices[t*3+1], i2 = io.indices[t*3+2];
        auto add = [&](u32 a, u32 b) {
            EdgeKey e{a < b ? a : b, a < b ? b : a};
            ++edge_count[e];
        };
        add(i0, i1); add(i1, i2); add(i2, i0);
    }

    // Collect boundary vertex indices.
    std::vector<bool> is_boundary(num_verts, false);
    for (const auto& [e, cnt] : edge_count) {
        if (cnt == 1) { is_boundary[e.a] = true; is_boundary[e.b] = true; }
    }

    struct BVert { u32 idx; math::v3 p; };
    std::vector<BVert> boundary;
    for (u32 i = 0; i < num_verts; ++i) {
        if (is_boundary[i]) boundary.push_back({i, io.positions[i]});
    }
    if (boundary.size() < 2) return 0;

    // Spatial hash grid.
    const f32 cell = stitch_distance;
    auto cell_of = [cell](f32 c) -> std::int64_t {
        return static_cast<std::int64_t>(std::floor(c / cell));
    };
    struct CellKey { std::int64_t x,y,z; bool operator==(const CellKey& o) const { return x==o.x&&y==o.y&&z==o.z; } };
    struct CellHash { size_t operator()(const CellKey& k) const noexcept {
        u32 h=0; primal::utl::MurmurHash3_x86_32(&k, sizeof(k), 0, &h); return (size_t)h;
    }};
    std::unordered_map<CellKey, std::vector<u32>, CellHash> grid;
    grid.reserve(boundary.size());
    for (u32 i = 0; i < (u32)boundary.size(); ++i) {
        const auto& p = boundary[i].p;
        grid[{cell_of(p.x), cell_of(p.y), cell_of(p.z)}].push_back(i);
    }

    const f32 dist_sq = stitch_distance * stitch_distance;
    std::vector<bool> dead(boundary.size(), false);
    u32 merges = 0;
    for (u32 i = 0; i < (u32)boundary.size(); ++i) {
        if (dead[i]) continue;
        const u32 survivor = boundary[i].idx;
        const math::v3& pi = boundary[i].p;
        std::int64_t cx = cell_of(pi.x), cy = cell_of(pi.y), cz = cell_of(pi.z);
        for (std::int64_t dx=-1;dx<=1;++dx)
        for (std::int64_t dy=-1;dy<=1;++dy)
        for (std::int64_t dz=-1;dz<=1;++dz) {
            auto it = grid.find({cx+dx, cy+dy, cz+dz});
            if (it == grid.end()) continue;
            for (u32 j : it->second) {
                if (j <= i || dead[j]) continue;
                const math::v3& pj = boundary[j].p;
                f32 ddsq = (pi.x-pj.x)*(pi.x-pj.x)+(pi.y-pj.y)*(pi.y-pj.y)+(pi.z-pj.z)*(pi.z-pj.z);
                if (ddsq > dist_sq) continue;
                out_remap[boundary[j].idx] = survivor;
                dead[j] = true;
                ++merges;
            }
        }
    }
    return merges;
}

// Apply a vertex remap (from compute_ir_boundary_remap) to the ProcessableMesh
// two corners collapsed to the same vertex), and compacts all attribute
// arrays to remove now-unreferenced vertices.
void apply_vertex_remap(ProcessableMesh& io, const std::vector<u32>& remap) {
    const u32 num_verts = (u32)io.positions.size();

    // Rewrite indices through remap, dropping degenerate triangles.
    u32 write = 0;
    for (u32 t = 0; t < (u32)io.indices.size(); t += 3) {
        u32 a = remap[io.indices[t]];
        u32 b = remap[io.indices[t + 1]];
        u32 c = remap[io.indices[t + 2]];
        if (a == b || b == c || c == a) continue;
        io.indices[write++] = a;
        io.indices[write++] = b;
        io.indices[write++] = c;
    }
    io.indices.resize(write);

    // Compact: build new index assignment for surviving vertices.
    std::vector<u32> compact(num_verts, u32_invalid_id);
    u32 next = 0;
    for (u32 i = 0; i < num_verts; ++i) {
        if (remap[i] == i) compact[i] = next++;  // survivor keeps its data
    }

    // Re-point indices to compacted range.
    for (auto& idx : io.indices) idx = compact[idx];

    // Compact attribute arrays — move only survivor entries into dense output.
    auto compact_v3 = [](utl::vector<math::v3>& arr,
                         const std::vector<u32>& compact_map) {
        utl::vector<math::v3> out;
        for (u32 i = 0; i < (u32)arr.size(); ++i) {
            if (compact_map[i] != u32_invalid_id) {
                if ((u32)out.size() <= compact_map[i]) out.resize(compact_map[i] + 1);
                out[compact_map[i]] = arr[i];
            }
        }
        arr = std::move(out);
    };
    auto compact_v4 = [](utl::vector<math::v4>& arr,
                         const std::vector<u32>& compact_map) {
        utl::vector<math::v4> out;
        for (u32 i = 0; i < (u32)arr.size(); ++i) {
            if (compact_map[i] != u32_invalid_id) {
                if ((u32)out.size() <= compact_map[i]) out.resize(compact_map[i] + 1);
                out[compact_map[i]] = arr[i];
            }
        }
        arr = std::move(out);
    };
    auto compact_v2 = [](utl::vector<math::v2>& arr,
                         const std::vector<u32>& compact_map) {
        utl::vector<math::v2> out;
        for (u32 i = 0; i < (u32)arr.size(); ++i) {
            if (compact_map[i] != u32_invalid_id) {
                if ((u32)out.size() <= compact_map[i]) out.resize(compact_map[i] + 1);
                out[compact_map[i]] = arr[i];
            }
        }
        arr = std::move(out);
    };
    compact_v3(io.positions, compact);
    compact_v3(io.normals, compact);
    compact_v4(io.tangents, compact);
    compact_v3(io.colors, compact);
    for (auto& s : io.uv_sets) compact_v2(s.coords, compact);
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
    // Flood-fill face orientation consistency using a two-pass strategy:
    //   Pass 1: BFS from face 0, marking which faces need flipping.
    //   Pass 2: collect all flip-candidates' vertex triples, delete them
    //           all, then re-add reversed, then garbage_collection once.
    // The old per-face delete+re-add interleaved with BFS corrupted PMP's
    // halfedge invariants and left winding un-propagated (flipped faces
    // were marked visited but never re-enqueued, so their neighbors were
    // never checked — causing meshlet computeClusterBounds dn<=0 asserts).
    if (m.n_faces() == 0) return 0;

    // Pass 1: BFS to determine flip set. We use face indices and the
    // adjacency from the CURRENT (unmodified) halfedge structure.
    // CRITICAL: arrays must be sized to faces_size() (property array capacity,
    // INCLUDING deleted faces), NOT n_faces(). PMP's face idx values occupy
    // [0, faces_size) but n_faces() excludes deleted — using n_faces for array
    // size causes OOB writes via need_flip[nb.idx()] / visited[...].
    std::vector<bool> visited(m.faces_size(), false);
    std::vector<bool> need_flip(m.faces_size(), false);
    std::vector<Face> queue;
    queue.reserve(m.n_faces());
    queue.push_back(*(m.faces_begin()));
    visited[queue.front().idx()] = true;

    while (!queue.empty()) {
        Face f = queue.back();
        queue.pop_back();
        const bool f_flipped = need_flip[f.idx()];
        for (auto h : m.halfedges(f)) {
            Halfedge opp = m.opposite_halfedge(h);
            Face  nb    = m.face(opp);
            if (!nb.is_valid()) continue;            // boundary
            if (visited[nb.idx()]) continue;
            // Consistent winding: the shared edge is traversed in opposite
            // directions by the two faces. If from_vertex(h)==from_vertex(opp)
            // they point the same way → inconsistent → nb needs flip relative
            // to f. Account for f's own flip state: if f is flipped, the
            // sense of "consistent" reverses for its neighbors.
            const bool inconsistent = (m.from_vertex(h) == m.from_vertex(opp));
            need_flip[nb.idx()] = inconsistent != f_flipped;
            visited[nb.idx()] = true;
            queue.push_back(nb);
        }
    }

    u32 flipped = 0;
    for (size_t i = 0; i < m.faces_size(); ++i) {
        if (need_flip[i]) ++flipped;
    }
    if (flipped == 0) return 0;

    // Pass 2: batch delete + re-add reversed. Collect vertex triples for
    // all faces that need flipping BEFORE deleting (handles invalidation).
    struct Tri { Vertex a, b, c; Face original; };
    std::vector<Tri> to_readd;
    for (auto f : m.faces()) {
        // Use f.idx() (PMP's actual face index), NOT a sequential counter:
        // the face iterator skips deleted faces, so a counter wouldn't align
        // with need_flip[] which is indexed by face idx.
        if (need_flip[f.idx()]) {
            unsigned i = 0;
            Vertex fv[3]{};
            for (auto v : m.vertices(f)) {
                if (i < 3) fv[i++] = v;
            }
            if (i == 3) to_readd.push_back({fv[0], fv[1], fv[2], f});
        }
    }

    // Delete all flip-candidates first (no re-add interleaved).
    for (auto& t : to_readd) m.delete_face(t.original);
    // Re-add reversed, all at once.
    for (auto& t : to_readd) m.add_triangle(t.a, t.c, t.b);
    m.garbage_collection();
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

    // stitch_borders is now fully IR-level: we compute the boundary vertex
    // remap AFTER from_pmp, operating on the IR's dense vertex indices (not
    // PMP's sparse vertex indices which include deleted entries). This avoids
    // both the PMP topology corruption (delete+re-add) and the idx-space
    // mismatch between PMP and IR.
    const u8 effective_ops = params.ops;  // no longer masked
    bool do_stitch = enabled(static_cast<Op>(effective_ops), Op::StitchBorders);

    AttributeChannels attrs;
    u32 dropped_non_manifold = 0;
    std::vector<bool> keep_mask = mark_non_manifold_faces(io, dropped_non_manifold);
    if (dropped_non_manifold > 0) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "repair.dropped_non_manifold",
            "repair: dropped " + std::to_string(dropped_non_manifold) +
            " non-manifold face(s) before processing (pmp requires 2-manifold input)",
            "repair"});
    }
    // Copy-init (not default + move-assign): pmp::SurfaceMesh move-assign
    // invalidates VertexProperty<T> handles because pmp uses raw pointers
    // into PropertyArray storage that don't get updated on move.
    SurfaceMesh m = to_pmp(io, attrs, keep_mask);

    bool modified = false;
    u32 stat_removed  = 0;
    u32 stat_stitched = 0;
    u32 stat_filled   = 0;
    u32 stat_flipped  = 0;

    if (enabled(static_cast<Op>(effective_ops), Op::RemoveDegenerateFaces)) {
        stat_removed = remove_degenerate_faces(m);
        modified |= (stat_removed > 0);
    }
    // NOTE: StitchBorders is handled AFTER from_pmp at the IR level (see below).
    if (enabled(static_cast<Op>(effective_ops), Op::FillHoles)) {
        stat_filled = fill_holes(m, params.max_hole_size);
        modified |= (stat_filled > 0);
    }
    if (params.orient_outward &&
        enabled(static_cast<Op>(effective_ops), Op::OrientOutward)) {
        stat_flipped = orient_outward(m);
        modified |= (stat_flipped > 0);
    }

    if (modified) {
        from_pmp(m, attrs, io);
        // Validate IR integrity: all indices must be < positions.size().
        // PMP garbage_collection after remove_degenerate_faces/fill_holes can
        // leave stale indices in rare edge cases. Drop bad triangles so
        // downstream modules (remesh) don't crash on OOB vertex access.
        {
            u32 w = 0;
            const u32 nv = (u32)io.positions.size();
            for (u32 r = 0; r < (u32)io.indices.size(); r += 3) {
                if (io.indices[r] < nv && io.indices[r+1] < nv && io.indices[r+2] < nv) {
                    io.indices[w++] = io.indices[r];
                    io.indices[w++] = io.indices[r+1];
                    io.indices[w++] = io.indices[r+2];
                }
            }
            io.indices.resize(w);
        }
        // Apply IR-level vertex remap from stitch_borders (after from_pmp so
        // StitchBorders: run at IR level after from_pmp. The IR uses dense
        // vertex indices [0, positions.size()), so the spatial hash + remap
        // operates on the same index space as io.indices — no PMP idx mismatch.
        if (do_stitch && params.stitch_distance > 0.f) {
            std::vector<u32> ir_remap;
            stat_stitched = compute_ir_boundary_remap(
                io, params.stitch_distance, ir_remap);
            if (stat_stitched > 0) {
                apply_vertex_remap(io, ir_remap);
            }
        }
        // Repair may have changed topology (fill_hole added verts, orient
        // flipped winding, stitch merged verts). Mark normals/tangents dirty
        // so downstream derive knows to recompute. PMP's vertex normals are
        // the pre-repair originals and don't reflect the new winding.
        io.normals_dirty = true;
        io.tangents_dirty = true;
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
