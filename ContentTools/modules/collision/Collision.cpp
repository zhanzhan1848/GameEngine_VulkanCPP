#include "Collision.h"

#include "VHACD.h"

namespace primal::tools::collision {
namespace {

VHACD::IVHACD::Parameters make_vhacd_params(const Params& p) {
    VHACD::IVHACD::Parameters v;
    v.Init();
    v.m_maxConvexHulls            = p.max_hulls;
    v.m_resolution                = p.voxel_resolution;
    v.m_concavity                 = (double)p.max_concavity;
    v.m_maxNumVerticesPerCH       = p.max_vertices_per_hull;
    v.m_pca                       = p.pca ? 1u : 0u;
    v.m_planeDownsampling         = p.plane_downsampling;
    v.m_convexhullDownsampling    = p.convexhull_downsampling;
    // No OpenCL / no OpenMP in our build — disable both explicitly.
    v.m_oclAcceleration           = 0u;
    v.m_convexhullApproximation   = 1u;
    v.m_projectHullVertices       = true;
    return v;
}

}  // namespace

// ---- Run -----------------------------------------------------------------

utl::vector<Hull> Run(const ProcessableMesh& m, const Params& params,
                      utl::vector<ErrorReport>& errors) {
    utl::vector<Hull> hulls;

    if (m.positions.empty() || m.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "collision.empty_input",
            "collision: empty input mesh, skipping", "collision"});
        return hulls;
    }
    if (m.indices.size() % 3 != 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "collision.bad_index_count",
            "collision: index count not divisible by 3", "collision"});
        return hulls;
    }
    if (params.max_hulls == 0 || params.voxel_resolution == 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "collision.invalid_params",
            "collision: max_hulls and voxel_resolution must be > 0",
            "collision"});
        return hulls;
    }

    VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
    if (!vhacd) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "collision.create_failed",
            "collision: VHACD::CreateVHACD returned null", "collision"});
        return hulls;
    }

    const VHACD::IVHACD::Parameters vp = make_vhacd_params(params);

    const bool ok = vhacd->Compute(
        reinterpret_cast<const float*>(m.positions.data()),
        static_cast<uint32_t>(m.positions.size()),
        m.indices.data(),
        static_cast<uint32_t>(m.indices.size() / 3),
        vp);

    if (!ok) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "collision.compute_failed",
            "collision: VHACD::Compute returned false", "collision"});
        vhacd->Clean();
        vhacd->Release();
        return hulls;
    }

    const uint32_t n = vhacd->GetNConvexHulls();
    hulls.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        VHACD::IVHACD::ConvexHull ch;
        vhacd->GetConvexHull(i, ch);
        if (ch.m_nPoints == 0 || ch.m_nTriangles == 0) continue;

        Hull h;
        h.vertices.reserve(ch.m_nPoints);
        for (uint32_t p = 0; p < ch.m_nPoints; ++p) {
            h.vertices.emplace_back(math::v3{
                (f32)ch.m_points[p * 3 + 0],
                (f32)ch.m_points[p * 3 + 1],
                (f32)ch.m_points[p * 3 + 2]});
        }
        h.indices.reserve(ch.m_nTriangles * 3);
        for (uint32_t t = 0; t < ch.m_nTriangles * 3; ++t) {
            h.indices.emplace_back(static_cast<u32>(ch.m_triangles[t]));
        }
        h.volume = (f32)ch.m_volume;
        h.center = math::v3{
            (f32)ch.m_center[0], (f32)ch.m_center[1], (f32)ch.m_center[2]};
        hulls.emplace_back(std::move(h));
    }

    vhacd->Clean();
    vhacd->Release();

    errors.emplace_back(ErrorReport{
        Severity::Info, "collision.ok",
        "collision: decomposed into " + std::to_string(hulls.size()) +
        " hulls", "collision"});
    return hulls;
}

}  // namespace primal::tools::collision
