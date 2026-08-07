#include "Remesh.h"

#include "pmp/surface_mesh.h"
#include "pmp/algorithms/remeshing.h"

#include <igl/qslim.h>
#include <Eigen/Core>
#include <Eigen/Dense>

#include <cmath>
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
    AttributeChannels attrs;
    SurfaceMesh m = to_pmp(io, attrs);
    try {
        if (params.mode == Mode::Isotropic) {
            pmp::uniform_remeshing(
                m, params.target_edge_length,
                static_cast<unsigned>(params.iterations),
                params.use_projection);
        } else if (params.mode == Mode::Adaptive) {
            // Adaptive takes a min/max range around target_edge_length.
            // Heuristic: min = target/2, max = target*2 — matches Botsch 2004.
            const pmp::Scalar t = params.target_edge_length;
            pmp::adaptive_remeshing(
                m, t * 0.5, t * 2.0, params.adaptive_approx_error,
                static_cast<unsigned>(params.iterations),
                params.use_projection);
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
