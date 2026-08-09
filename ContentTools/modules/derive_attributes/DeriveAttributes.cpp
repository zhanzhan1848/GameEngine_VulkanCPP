#include "DeriveAttributes.h"
#include "MikkTSpaceAdapter.h"
#include "mikktspace.h"

#include <cmath>
#include <vector>

namespace primal::tools::derive {
namespace {

using primal::math::v2;
using primal::math::v3;
using primal::math::v4;
using primal::tools::Severity;
using primal::tools::ErrorReport;
using primal::utl::vector;

// ---- face normal --------------------------------------------------------

inline v3 cross_v3(const v3& a, const v3& b) {
    return v3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}
inline f32 dot_v3(const v3& a, const v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline f32 length_v3(const v3& v) {
    return std::sqrt(dot_v3(v, v));
}
inline v3 normalize_v3(const v3& v) {
    const f32 L = length_v3(v);
    if (L < 1e-20f) return v3{0.f, 0.f, 0.f};
    const f32 inv = 1.f / L;
    return v3{v.x * inv, v.y * inv, v.z * inv};
}

// Compute per-face normal. Returns zero vector for degenerate triangles.
v3 face_normal(const ProcessableMesh& m, u32 tri_idx) {
    const u32 i0 = m.indices[tri_idx * 3 + 0];
    const u32 i1 = m.indices[tri_idx * 3 + 1];
    const u32 i2 = m.indices[tri_idx * 3 + 2];
    const v3 e0 = v3{m.positions[i1].x - m.positions[i0].x,
                     m.positions[i1].y - m.positions[i0].y,
                     m.positions[i1].z - m.positions[i0].z};
    const v3 e1 = v3{m.positions[i2].x - m.positions[i0].x,
                     m.positions[i2].y - m.positions[i0].y,
                     m.positions[i2].z - m.positions[i0].z};
    return normalize_v3(cross_v3(e0, e1));
}

// Angle at vertex `corner_local` (0/1/2) of triangle `tri_idx`, in radians.
f32 vertex_angle(const ProcessableMesh& m, u32 tri_idx, u32 corner_local) {
    const u32 idx[3] = {
        m.indices[tri_idx * 3 + 0],
        m.indices[tri_idx * 3 + 1],
        m.indices[tri_idx * 3 + 2],
    };
    const u32 a = idx[(corner_local + 1) % 3];
    const u32 b = idx[(corner_local + 2) % 3];
    const u32 c = idx[corner_local];  // the vertex we measure the angle at
    const v3 e0 = normalize_v3(v3{m.positions[a].x - m.positions[c].x,
                                  m.positions[a].y - m.positions[c].y,
                                  m.positions[a].z - m.positions[c].z});
    const v3 e1 = normalize_v3(v3{m.positions[b].x - m.positions[c].x,
                                  m.positions[b].y - m.positions[c].y,
                                  m.positions[b].z - m.positions[c].z});
    const f32 d = dot_v3(e0, e1);
    if (d >  1.f) return 0.f;
    if (d < -1.f) return primal::math::pi;
    return std::acos(d);
}

// ---- non-faceted normal: smooth / area / angle --------------------------
// All three keep positions.size() unchanged. Differences are only the weight
// applied to each adjacent face normal during accumulation.

void compute_smooth_normals(ProcessableMesh& m, NormalMode mode) {
    const u32 num_verts = (u32)m.positions.size();
    const u32 num_tris  = (u32)m.indices.size() / 3;
    m.normals.assign(num_verts, v3{0.f, 0.f, 0.f});

    for (u32 t = 0; t < num_tris; ++t) {
        const v3 n = face_normal(m, t);
        f32 weight = 1.f;
        for (u32 c = 0; c < 3; ++c) {
            const u32 v = m.indices[t * 3 + c];
            if (mode == NormalMode::AngleWeighted) {
                weight = vertex_angle(m, t, c);
            } else if (mode == NormalMode::AreaWeighted) {
                // weight stays 1 — but we use the un-normalized cross product
                // length, which is 2× triangle area. That makes this branch
                // fall through with weight=1 and use the raw cross below.
            }
            // For Smooth: weight=1, n already normalized → simple average.
            // For AreaWeighted: we want un-normalized cross (2×area).
            if (mode == NormalMode::AreaWeighted) {
                // Recompute unnormalized cross for area weighting.
                const u32 i0 = m.indices[t * 3 + 0];
                const u32 i1 = m.indices[t * 3 + 1];
                const u32 i2 = m.indices[t * 3 + 2];
                const v3 e0 = v3{m.positions[i1].x - m.positions[i0].x,
                                 m.positions[i1].y - m.positions[i0].y,
                                 m.positions[i1].z - m.positions[i0].z};
                const v3 e1 = v3{m.positions[i2].x - m.positions[i0].x,
                                 m.positions[i2].y - m.positions[i0].y,
                                 m.positions[i2].z - m.positions[i0].z};
                const v3 area_n = cross_v3(e0, e1);
                m.normals[v] = v3{m.normals[v].x + area_n.x,
                                  m.normals[v].y + area_n.y,
                                  m.normals[v].z + area_n.z};
            } else {
                m.normals[v] = v3{m.normals[v].x + n.x * weight,
                                  m.normals[v].y + n.y * weight,
                                  m.normals[v].z + n.z * weight};
            }
        }
    }
    for (u32 v = 0; v < num_verts; ++v) {
        m.normals[v] = normalize_v3(m.normals[v]);
    }
}

// ---- faceted normal: angle-threshold vertex duplication ----------------

// One slot per (vertex, smooth-group) pair. We linearly scan existing slots
// for the vertex; if any has a normal within `cos_threshold` of the new
// face normal, reuse it. Otherwise create a new slot.
struct Slot {
    v3  normal;
};

struct VertSlots {
    vector<u32> slot_indices;  // indices into the global slot array
};

void compute_faceted_normals(ProcessableMesh& m, f32 angle_degrees) {
    const u32 num_verts = (u32)m.positions.size();
    const u32 num_tris  = (u32)m.indices.size() / 3;
    const f32 cos_threshold = std::cos(angle_degrees * primal::math::pi / 180.f);

    // Per original vertex: list of slots (each slot has its accumulated normal).
    std::vector<std::vector<u32>> vert_slot_list(num_verts);
    std::vector<Slot> slots;

    // Pre-compute face normals.
    std::vector<v3> face_normals(num_tris);
    for (u32 t = 0; t < num_tris; ++t) face_normals[t] = face_normal(m, t);

    // First pass: assign each (vertex, face) corner to a slot.
    // slot_lookup[tri_idx * 3 + corner] = slot_index
    std::vector<u32> slot_lookup(num_tris * 3, u32_invalid_id);

    for (u32 t = 0; t < num_tris; ++t) {
        const v3 n = face_normals[t];
        for (u32 c = 0; c < 3; ++c) {
            const u32 v = m.indices[t * 3 + c];
            u32 chosen_slot = u32_invalid_id;
            for (u32 s : vert_slot_list[v]) {
                if (dot_v3(slots[s].normal, n) >= cos_threshold - 1e-6f) {
                    chosen_slot = s;
                    break;
                }
            }
            if (chosen_slot == u32_invalid_id) {
                chosen_slot = (u32)slots.size();
                slots.push_back(Slot{n});
                vert_slot_list[v].push_back(chosen_slot);
            } else {
                // Accumulate weighted by area (use un-normalized face normal
                // for stable weighting). We re-fetch the un-normalized normal.
                const u32 i0 = m.indices[t * 3 + 0];
                const u32 i1 = m.indices[t * 3 + 1];
                const u32 i2 = m.indices[t * 3 + 2];
                const v3 e0 = v3{m.positions[i1].x - m.positions[i0].x,
                                 m.positions[i1].y - m.positions[i0].y,
                                 m.positions[i1].z - m.positions[i0].z};
                const v3 e1 = v3{m.positions[i2].x - m.positions[i0].x,
                                 m.positions[i2].y - m.positions[i0].y,
                                 m.positions[i2].z - m.positions[i0].z};
                const v3 area_n = cross_v3(e0, e1);
                slots[chosen_slot].normal = v3{
                    slots[chosen_slot].normal.x + area_n.x,
                    slots[chosen_slot].normal.y + area_n.y,
                    slots[chosen_slot].normal.z + area_n.z};
            }
            slot_lookup[t * 3 + c] = chosen_slot;
        }
    }

    // Build the new vertex pool: one entry per slot.
    ProcessableMesh out;
    out.name = m.name;
    out.material_idx = m.material_idx;
    out.positions.reserve(slots.size());
    out.normals.reserve(slots.size());
    out.indices.resize(num_tris * 3);

    // Reverse lookup: for each original vertex, map slot → new output index.
    // We construct output by iterating slots in order, so slot i → out_idx i.
    for (u32 s = 0; s < slots.size(); ++s) {
        // Find which original vertex this slot belongs to (first match).
        u32 src_vert = u32_invalid_id;
        for (u32 v = 0; v < num_verts; ++v) {
            for (u32 sv : vert_slot_list[v]) {
                if (sv == s) { src_vert = v; break; }
            }
            if (src_vert != u32_invalid_id) break;
        }
        out.positions.push_back(m.positions[src_vert]);
        out.normals.push_back(normalize_v3(slots[s].normal));
        if (!m.tangents.empty())     out.tangents.push_back(m.tangents[src_vert]);
        if (!m.colors.empty())       out.colors.push_back(m.colors[src_vert]);
        for (const auto& src_uv : m.uv_sets) {
            if (out.uv_sets.size() < m.uv_sets.size()) {
                out.uv_sets.resize(m.uv_sets.size());
                out.uv_sets.back().purpose = src_uv.purpose;
            }
            if (!src_uv.coords.empty()) {
                out.uv_sets[(&src_uv - &m.uv_sets[0])].coords.push_back(
                    src_uv.coords[src_vert]);
            }
        }
    }
    for (u32 t = 0; t < num_tris; ++t) {
        for (u32 c = 0; c < 3; ++c) {
            out.indices[t * 3 + c] = slot_lookup[t * 3 + c];
        }
    }
    m = std::move(out);
}

// ---- area-weighted tangent ---------------------------------------------

void compute_area_weighted_tangent(ProcessableMesh& m) {
    const u32 num_verts = (u32)m.positions.size();
    if (m.uv_sets.empty() || m.uv_sets[0].coords.empty()) {
        m.tangents.clear();
        return;
    }
    const auto& uvs = m.uv_sets[0].coords;
    m.tangents.assign(num_verts, v4{0.f, 0.f, 0.f, 1.f});

    // Standard Gram-Schmidt tangent accumulation from UV derivatives.
    std::vector<v3> t1(num_verts, v3{0.f, 0.f, 0.f});
    std::vector<v3> t2(num_verts, v3{0.f, 0.f, 0.f});

    const u32 num_tris = (u32)m.indices.size() / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        const u32 i0 = m.indices[t * 3 + 0];
        const u32 i1 = m.indices[t * 3 + 1];
        const u32 i2 = m.indices[t * 3 + 2];

        const v3& p0 = m.positions[i0];
        const v3& p1 = m.positions[i1];
        const v3& p2 = m.positions[i2];
        const v2& w0 = uvs[i0];
        const v2& w1 = uvs[i1];
        const v2& w2 = uvs[i2];

        const v3 dp1 = v3{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const v3 dp2 = v3{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        const v2 duv1 = v2{w1.x - w0.x, w1.y - w0.y};
        const v2 duv2 = v2{w2.x - w0.x, w2.y - w0.y};

        const f32 det = duv1.x * duv2.y - duv2.x * duv1.y;
        const f32 r = (std::abs(det) > 1e-20f) ? (1.f / det) : 0.f;
        const f32 r1 =  duv2.y * r;
        const f32 r2 = -duv2.x * r;
        const f32 s1 = -duv1.y * r;
        const f32 s2 =  duv1.x * r;

        const v3 t_dir = v3{
            r1 * dp1.x + r2 * dp2.x,
            r1 * dp1.y + r2 * dp2.y,
            r1 * dp1.z + r2 * dp2.z,
        };
        const v3 b_dir = v3{
            s1 * dp1.x + s2 * dp2.x,
            s1 * dp1.y + s2 * dp2.y,
            s1 * dp1.z + s2 * dp2.z,
        };
        t1[i0] = v3{t1[i0].x + t_dir.x, t1[i0].y + t_dir.y, t1[i0].z + t_dir.z};
        t1[i1] = v3{t1[i1].x + t_dir.x, t1[i1].y + t_dir.y, t1[i1].z + t_dir.z};
        t1[i2] = v3{t1[i2].x + t_dir.x, t1[i2].y + t_dir.y, t1[i2].z + t_dir.z};
        t2[i0] = v3{t2[i0].x + b_dir.x, t2[i0].y + b_dir.y, t2[i0].z + b_dir.z};
        t2[i1] = v3{t2[i1].x + b_dir.x, t2[i1].y + b_dir.y, t2[i1].z + b_dir.z};
        t2[i2] = v3{t2[i2].x + b_dir.x, t2[i2].y + b_dir.y, t2[i2].z + b_dir.z};
    }
    for (u32 v = 0; v < num_verts; ++v) {
        const v3& n = (!m.normals.empty()) ? m.normals[v] : v3{0.f, 1.f, 0.f};
        const v3 t = t1[v];
        // Gram-Schmidt: t' = normalize(t - n * dot(n, t))
        const f32 d = dot_v3(n, t);
        const v3 t_orth = v3{t.x - n.x * d, t.y - n.y * d, t.z - n.z * d};
        const v3 t_norm = normalize_v3(t_orth);
        // Sign from (cross(n, t) · t2).
        const v3 b_approx = cross_v3(n, t_norm);
        const f32 sign = (dot_v3(b_approx, t2[v]) < 0.f) ? -1.f : 1.f;
        m.tangents[v] = v4{t_norm.x, t_norm.y, t_norm.z, sign};
    }
}

// ---- MikkTSpace tangent -------------------------------------------------

u32 find_texture_uv_set(const ProcessableMesh& m) {
    for (u32 i = 0; i < (u32)m.uv_sets.size(); ++i) {
        if (m.uv_sets[i].purpose == primal::tools::UVSetPurpose::Texture &&
            !m.uv_sets[i].coords.empty()) {
            return i;
        }
    }
    // Fallback: first non-empty UVSet of any purpose.
    for (u32 i = 0; i < (u32)m.uv_sets.size(); ++i) {
        if (!m.uv_sets[i].coords.empty()) return i;
    }
    return u32_invalid_id;
}

bool compute_mikktspace_tangent(ProcessableMesh& m,
                                vector<ErrorReport>& errors) {
    const u32 uv_idx = find_texture_uv_set(m);
    if (uv_idx == u32_invalid_id) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "derive.mikktspace_no_uv",
            "derive: MikkTSpace requested but mesh has no UV; "
            "falling back to AreaWeighted tangent",
            "derive"});
        compute_area_weighted_tangent(m);
        return false;
    }
    if (m.normals.empty()) {
        // MikkTSpace queries m_getNormal; we return up-vector if missing.
        errors.emplace_back(ErrorReport{
            Severity::Warning, "derive.mikktspace_no_normals",
            "derive: MikkTSpace queried with empty normals; "
            "results may be inaccurate",
            "derive"});
    }
    m.tangents.assign(m.positions.size(), v4{0.f, 0.f, 0.f, 1.f});

    SMikkTSpaceContext* ctx = create_context(m, uv_idx);
    const tbool ok = genTangSpaceDefault(ctx);
    destroy_context(ctx);
    if (!ok) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "derive.mikktspace_failed",
            "derive: genTangSpaceDefault returned false", "derive"});
        return false;
    }
    return true;
}

}  // namespace

// ---- Run ----------------------------------------------------------------

bool Run(ProcessableMesh& io, const Params& params,
         vector<ErrorReport>& errors) {
    if (io.positions.empty() || io.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "derive.empty_input",
            "derive: empty input mesh, skipping", "derive"});
        return false;
    }

    // Step 1: normals.
    switch (params.normal_mode) {
        case NormalMode::Smooth:
        case NormalMode::AreaWeighted:
        case NormalMode::AngleWeighted:
            compute_smooth_normals(io, params.normal_mode);
            break;
        case NormalMode::Faceted:
            compute_faceted_normals(io, params.faceted_angle_degrees);
            break;
    }

    // Step 2: tangents.
    switch (params.tangent_mode) {
        case TangentMode::None:
            io.tangents.clear();
            break;
        case TangentMode::AreaWeighted:
            compute_area_weighted_tangent(io);
            break;
        case TangentMode::MikkTSpace:
            compute_mikktspace_tangent(io, errors);
            break;
    }

    errors.emplace_back(ErrorReport{
        Severity::Info, "derive.ok",
        std::string("derive: normal=") +
        (params.normal_mode == NormalMode::Faceted    ? "Faceted"    :
         params.normal_mode == NormalMode::Smooth     ? "Smooth"     :
         params.normal_mode == NormalMode::AreaWeighted ? "AreaWeighted" :
         "AngleWeighted") +
        " tangent=" +
        (params.tangent_mode == TangentMode::None        ? "None"        :
         params.tangent_mode == TangentMode::AreaWeighted ? "AreaWeighted" :
         "MikkTSpace") +
        " done", "derive"});
    return true;
}

}  // namespace primal::tools::derive
