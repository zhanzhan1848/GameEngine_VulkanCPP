#include "Validator.h"
#include "SceneBlobReader.h"

#include <cmath>

namespace primal::tools::pipeline {
namespace {

bool is_zero_bounds(const mesh::meshlet& m) {
    if (m.radius <= 0.f || !std::isfinite(m.radius)) return true;
    if (m.center[0] == 0.f && m.center[1] == 0.f && m.center[2] == 0.f) {
        // Not necessarily invalid but worth flagging if many meshlets cluster at origin.
        return false;  // we accept origin-centered meshlets
    }
    return false;
}

void validate_meshlet(const mesh::meshlet& m, u32 meshlet_vertex_count,
                      u32 meshlet_triangle_count,
                      ValidationReport& report,
                      utl::vector<ErrorReport>& errors,
                      const std::string& mesh_name, u32 meshlet_idx) {
    const std::string loc = mesh_name + "[" + std::to_string(meshlet_idx) + "]";

    if (m.vertex_count > kRuntimeMaxMeshletVertices) {
        ++report.meshlets_over_vertex_cap;
        ++report.total_errors;
        errors.emplace_back(ErrorReport{Severity::Error, "validate.over_vertex_cap",
            "validate: " + loc + " vertex_count " + std::to_string(m.vertex_count) +
            " > " + std::to_string(kRuntimeMaxMeshletVertices), "validate"});
    }
    if (m.triangle_count > kRuntimeMaxMeshletTriangles) {
        ++report.meshlets_over_triangle_cap;
        ++report.total_errors;
        errors.emplace_back(ErrorReport{Severity::Error, "validate.over_triangle_cap",
            "validate: " + loc + " triangle_count " + std::to_string(m.triangle_count) +
            " > " + std::to_string(kRuntimeMaxMeshletTriangles), "validate"});
    }

    // Offset bounds: meshlet's vertex/triangle window must fit within the
    // mesh-global meshlet_vertices/meshlet_triangles arrays.
    if (m.vertex_offset + m.vertex_count > meshlet_vertex_count) {
        ++report.meshlets_invalid_offsets;
        ++report.total_errors;
        errors.emplace_back(ErrorReport{Severity::Error, "validate.bad_vertex_offset",
            "validate: " + loc + " vertex window out of bounds", "validate"});
    }
    // Triangle byte length is exactly triangle_count*3 — Geometry.cpp's
    // process_meshlets:280 packs triangles with no inter-meshlet padding.
    const u32 tri_bytes = m.triangle_count * 3u;
    if (m.triangle_offset + tri_bytes > meshlet_triangle_count) {
        ++report.meshlets_invalid_offsets;
        ++report.total_errors;
        errors.emplace_back(ErrorReport{Severity::Error, "validate.bad_triangle_offset",
            "validate: " + loc + " triangle window out of bounds", "validate"});
    }

    // Triangle indices (local to meshlet's vertex window) must be < vertex_count.
    // We can't check this here without the raw triangle bytes — see
    // ValidateMeshletsFull in a future revision.
    if (m.vertex_count == 0 || m.triangle_count == 0) {
        ++report.meshlets_invalid_offsets;
        ++report.total_warnings;
        errors.emplace_back(ErrorReport{Severity::Warning, "validate.empty_meshlet",
            "validate: " + loc + " empty meshlet (vertex_count=0 || triangle_count=0)",
            "validate"});
    }

    if (is_zero_bounds(m)) {
        ++report.meshlets_zero_bounds;
        ++report.total_errors;
        errors.emplace_back(ErrorReport{Severity::Error, "validate.zero_bounds",
            "validate: " + loc + " radius<=0 or non-finite", "validate"});
    }
}

}  // namespace

bool ValidateSceneBlob(const scene_data& data,
                       ValidationReport& report,
                       utl::vector<ErrorReport>& errors) {
    auto visitor = [&](u32 mesh_idx, const PackedMeshView& view) {
        ++report.mesh_count;
        report.meshlet_count += view.meshlet_count;

        for (u32 i = 0; i < view.meshlet_count; ++i) {
            validate_meshlet(view.meshlets[i],
                             view.meshlet_vertex_count,
                             view.meshlet_triangle_count,
                             report, errors, view.name, i);
        }
    };

    WalkSceneBlob(data, visitor, errors);
    // Any Error severity (parse failure or validation error) → fail.
    // Warnings (e.g. empty buffer) alone are acceptable.
    for (const auto& e : errors) {
        if (e.severity == Severity::Error) return false;
    }
    return true;
}

}  // namespace primal::tools::pipeline
