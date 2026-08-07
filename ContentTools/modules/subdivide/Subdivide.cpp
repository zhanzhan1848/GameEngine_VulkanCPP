#include "Subdivide.h"

#include "pmp/surface_mesh.h"
#include "pmp/algorithms/subdivision.h"

namespace primal::tools::subdivide {
namespace {

using pmp::SurfaceMesh;
using pmp::Vertex;
using pmp::Point;

// ---- IR ↔ PMP bridge (duplicated from Repair.cpp / Remesh.cpp; Phase 1
//      keeps per-TU anonymous-namespace isolation per the project
//      convention. M8 pipeline-composition layer will factor the bridge
//      into modules/_shared/PmpBridge.h if duplication grows past 3 TUs.) ----

struct AttributeChannels {
    pmp::VertexProperty<math::v3>*  normals {nullptr};
    pmp::VertexProperty<math::v4>*  tangents{nullptr};
    pmp::VertexProperty<math::v3>*  colors  {nullptr};
    std::vector<pmp::VertexProperty<math::v2>*> uvs;
};

SurfaceMesh to_pmp(const ProcessableMesh& ir, AttributeChannels& attrs) {
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

    const u32 num_tris = (u32)ir.indices.size() / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        const u32 i0 = ir.indices[t * 3 + 0];
        const u32 i1 = ir.indices[t * 3 + 1];
        const u32 i2 = ir.indices[t * 3 + 2];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;
        m.add_triangle(Vertex(i0), Vertex(i1), Vertex(i2));
    }
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

}  // namespace

// ---- Run -----------------------------------------------------------------

bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors) {
    if (io.positions.empty() || io.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "subdivide.empty_input",
            "subdivide: empty input mesh, skipping", "subdivide"});
        return false;
    }
    if (params.levels == 0) {
        // No-op; not an error.
        return true;
    }

    AttributeChannels attrs;
    SurfaceMesh m = to_pmp(io, attrs);
    try {
        for (u32 i = 0; i < params.levels; ++i) {
            if (params.scheme == Scheme::Loop) {
                pmp::loop_subdivision(m);
            } else {
                pmp::catmull_clark_subdivision(m);
            }
        }
    } catch (const std::exception& e) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "subdivide.pmp_exception",
            std::string("subdivide: PMP threw: ") + e.what(), "subdivide"});
        release_attrs(attrs);
        return false;
    } catch (...) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "subdivide.pmp_exception",
            "subdivide: PMP threw unknown exception", "subdivide"});
        release_attrs(attrs);
        return false;
    }

    from_pmp(m, attrs, io);
    release_attrs(attrs);
    errors.emplace_back(ErrorReport{
        Severity::Info, "subdivide.ok",
        std::string("subdivide: ") +
        (params.scheme == Scheme::Loop ? "Loop" : "CatmullClark") +
        " x" + std::to_string(params.levels) + " done", "subdivide"});
    return true;
}

}  // namespace primal::tools::subdivide
