#include "UvAtlas.h"

#include "xatlas.h"

#include <cmath>
#include <vector>

namespace primal::tools::uvatlas {
namespace {

// ---- IR ↔ xatlas bridge --------------------------------------------------
//
// xatlas takes position + index arrays (positions via stride, so math::v3's
// 16-byte simd::float3 padding is fine — stride=16, xatlas reads 3 floats).
// Output Vertex carries `xref` = index into the INPUT vertex array. We use
// that to pull positions/normals/tangents/colors AND every existing uv_sets
// entry through the new topology.
//
// Note: math::v3 (simd::float3) has sizeof=16 on Apple Silicon. math::v2
// (simd::float2) has sizeof=8 (no padding). xatlas reads via stride so the
// padding bytes are simply skipped.

void fill_mesh_decl(xatlas::MeshDecl& decl, const ProcessableMesh& ir,
                    const UVSet* uv_hint) {
    decl.vertexCount         = static_cast<uint32_t>(ir.positions.size());
    decl.vertexPositionData  = ir.positions.data();
    decl.vertexPositionStride = sizeof(math::v3);
    if (uv_hint && !uv_hint->coords.empty()) {
        decl.vertexUvData    = uv_hint->coords.data();
        decl.vertexUvStride  = sizeof(math::v2);
    } else {
        decl.vertexUvData    = nullptr;
        decl.vertexUvStride  = 0;
    }
    decl.indexCount   = static_cast<uint32_t>(ir.indices.size());
    decl.indexData    = ir.indices.data();
    decl.indexFormat  = xatlas::IndexFormat::UInt32;
    decl.indexOffset  = 0;
}

xatlas::ChartOptions chart_options_for(const Params& p) {
    xatlas::ChartOptions o;
    if (p.target_purpose == UVSetPurpose::Lightmap) {
        // Stricter face winding + higher seam weights to produce clean chart
        // boundaries for the runtime lightmap integrator.
        o.fixWinding        = p.strict_unique_pack;
        o.normalSeamWeight  = 4.0f;
        o.textureSeamWeight = 2.0f;
    }
    if (p.target_purpose == UVSetPurpose::Texture && p.use_input_uvs_as_hint) {
        // Honor artist-cut seams where present.
        o.useInputMeshUvs = true;
    }
    if (p.max_chart_count > 0) {
        // xatlas has no direct chart count cap; we approximate by raising
        // maxCost so charts grow larger (fewer of them).
        o.maxCost = 4.0f;
    }
    return o;
}

xatlas::PackOptions pack_options_for(const Params& p) {
    xatlas::PackOptions o;
    o.padding          = static_cast<uint32_t>(std::max<f32>(0.f, p.gutter));
    o.rotateChartsToAxis = p.stretch_optimization;
    o.rotateCharts      = p.stretch_optimization;
    o.bruteForce        = p.strict_unique_pack;
    // For Lightmap we disable bilinear padding texels — runtime integrator
    // gathers with explicit margins, and the strict atlas region must
    // contain only valid texels.
    o.bilinear          = !p.strict_unique_pack;
    o.blockAlign        = false;
    o.createImage       = false;
    if (p.resolution_hint > 0) o.resolution = p.resolution_hint;
    if (p.texels_per_unit > 0.f) o.texelsPerUnit = p.texels_per_unit;
    return o;
}

// Rewrite ProcessableMesh from xatlas output. Pulls every per-vertex
// attribute through ov.xref; copies every existing uv_sets entry through
// the same map; finally writes the freshly-generated UV into the matching
// purpose-tagged UVSet (creating one if absent).
bool harvest_output(const xatlas::Atlas& atlas, const ProcessableMesh& src,
                    const Params& params, ProcessableMesh& out,
                    utl::vector<ErrorReport>& errors) {
    if (atlas.meshCount == 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.no_output",
            "uvatlas: xatlas produced zero output meshes", "uvatlas"});
        return false;
    }
    const xatlas::Mesh& om = atlas.meshes[0];

    out.name         = src.name;
    out.material_idx = src.material_idx;

    out.positions.reserve(om.vertexCount);
    for (uint32_t i = 0; i < om.vertexCount; ++i) {
        const uint32_t xi = om.vertexArray[i].xref;
        if (xi >= src.positions.size()) {
            errors.emplace_back(ErrorReport{
                Severity::Error, "uvatlas.xref_out_of_range",
                "uvatlas: output vertex xref exceeds input vertex count",
                "uvatlas"});
            return false;
        }
        out.positions.emplace_back(src.positions[xi]);
        if (!src.normals.empty())  out.normals.emplace_back(src.normals[xi]);
        if (!src.tangents.empty()) out.tangents.emplace_back(src.tangents[xi]);
        if (!src.colors.empty())   out.colors.emplace_back(src.colors[xi]);
    }

    out.indices.reserve(om.indexCount);
    for (uint32_t i = 0; i < om.indexCount; ++i) {
        out.indices.emplace_back(static_cast<u32>(om.indexArray[i]));
    }

    // Carry over existing UV sets — each coord array is permuted via xref.
    // Important: this runs BEFORE we add the new target UVSet, so the new
    // generated channel doesn't get echoed back into itself.
    out.uv_sets.reserve(src.uv_sets.size() + 1);
    for (const auto& s : src.uv_sets) {
        out.uv_sets.emplace_back();
        UVSet& dst_set = out.uv_sets.back();
        dst_set.purpose = s.purpose;
        if (s.coords.empty()) continue;
        dst_set.coords.reserve(om.vertexCount);
        for (uint32_t i = 0; i < om.vertexCount; ++i) {
            const uint32_t xi = om.vertexArray[i].xref;
            dst_set.coords.emplace_back(s.coords[xi]);
        }
    }

    // Place generated UVs into the first UVSet matching target_purpose;
    // append a new UVSet if none exists yet.
    UVSet* target_set = nullptr;
    for (auto& s : out.uv_sets) {
        if (s.purpose == params.target_purpose) { target_set = &s; break; }
    }
    if (!target_set) {
        out.uv_sets.emplace_back();
        target_set = &out.uv_sets.back();
        target_set->purpose = params.target_purpose;
    }
    target_set->coords.clear();
    target_set->coords.reserve(om.vertexCount);

    // xatlas UVs are in atlas pixel space [0..width, 0..height]. Normalize
    // to [0,1] so downstream consumers (lightmap integrator, texture
    // sampler) can use them directly.
    const f32 inv_w = atlas.width  > 0 ? 1.0f / static_cast<f32>(atlas.width)  : 0.f;
    const f32 inv_h = atlas.height > 0 ? 1.0f / static_cast<f32>(atlas.height) : 0.f;
    for (uint32_t i = 0; i < om.vertexCount; ++i) {
        const xatlas::Vertex& ov = om.vertexArray[i];
        target_set->coords.emplace_back(math::v2{
            ov.uv[0] * inv_w,
            ov.uv[1] * inv_h,
        });
    }
    return true;
}

}  // namespace

// ---- Run -----------------------------------------------------------------

bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors) {
    if (io.positions.empty() || io.indices.empty()) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "uvatlas.empty_input",
            "uvatlas: empty input mesh, skipping", "uvatlas"});
        return false;
    }
    if (io.indices.size() % 3 != 0) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.bad_index_count",
            "uvatlas: index count is not divisible by 3", "uvatlas"});
        return false;
    }

    xatlas::Atlas* atlas = xatlas::Create();
    if (!atlas) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.create_failed",
            "uvatlas: xatlas::Create returned null", "uvatlas"});
        return false;
    }

    // For Texture purpose, the existing Texture-purpose UVSet (if any) is
    // passed as a chart hint. For Lightmap/Detail we deliberately ignore
    // input UVs to avoid biasing the new unwrap.
    const UVSet* uv_hint = nullptr;
    if (params.target_purpose == UVSetPurpose::Texture &&
        params.use_input_uvs_as_hint) {
        uv_hint = find_uv_set(io, UVSetPurpose::Texture);
    }

    xatlas::MeshDecl decl;
    fill_mesh_decl(decl, io, uv_hint);

    const xatlas::AddMeshError add_err = xatlas::AddMesh(atlas, decl, 1);
    if (add_err != xatlas::AddMeshError::Success) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.add_mesh_failed",
            std::string("uvatlas: AddMesh failed: ") +
            xatlas::StringForEnum(add_err), "uvatlas"});
        xatlas::Destroy(atlas);
        return false;
    }
    xatlas::AddMeshJoin(atlas);

    const xatlas::ChartOptions chart_opts = chart_options_for(params);
    const xatlas::PackOptions  pack_opts  = pack_options_for(params);

    try {
        xatlas::Generate(atlas, chart_opts, pack_opts);
    } catch (const std::exception& e) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.generate_exception",
            std::string("uvatlas: xatlas::Generate threw: ") + e.what(),
            "uvatlas"});
        xatlas::Destroy(atlas);
        return false;
    } catch (...) {
        errors.emplace_back(ErrorReport{
            Severity::Error, "uvatlas.generate_exception",
            "uvatlas: xatlas::Generate threw unknown exception", "uvatlas"});
        xatlas::Destroy(atlas);
        return false;
    }

    ProcessableMesh out;
    const bool ok = harvest_output(*atlas, io, params, out, errors);
    xatlas::Destroy(atlas);
    if (!ok) return false;

    // Validate invariants before swap so callers don't see partial state.
    const u32 nv = (u32)out.positions.size();
    if (!out.normals.empty()  && out.normals.size()  != nv) { /* shouldn't happen */ }
    if (!out.tangents.empty() && out.tangents.size() != nv) { /* shouldn't happen */ }
    for (const auto& s : out.uv_sets) {
        if (!s.coords.empty() && s.coords.size() != nv) {
            errors.emplace_back(ErrorReport{
                Severity::Error, "uvatlas.invariant_violated",
                "uvatlas: post-harvest UVSet size mismatch", "uvatlas"});
            return false;
        }
    }

    errors.emplace_back(ErrorReport{
        Severity::Info, "uvatlas.ok",
        std::string("uvatlas: ") +
        (params.target_purpose == UVSetPurpose::Texture  ? "Texture"  :
         params.target_purpose == UVSetPurpose::Lightmap ? "Lightmap" :
         params.target_purpose == UVSetPurpose::Detail   ? "Detail"   : "Custom") +
        " UV generated (" + std::to_string(out.positions.size()) +
        " verts, " + std::to_string(out.indices.size() / 3) + " tris)",
        "uvatlas"});
    io = std::move(out);
    return true;
}

}  // namespace primal::tools::uvatlas
