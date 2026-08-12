#pragma once

// Structured diagnostic channel for Phase 1 pipeline modules. Each module
// emits ErrorReports instead of throwing — AssetPipeline (M8) collects them
// into Result.warnings and returns to the C ABI caller.
//
// M3-M7 modules fill `code` with a stable machine-readable identifier
// (e.g. "repair.hole_fill_failed") so the Editor / batch tools can filter
// without parsing human-readable text.

// ToolsCommon.h is the project convention header (matches Geometry.h,
// PrimitiveMesh.h, etc.) even when only primitive types are needed.
#include "ToolsCommon.h"

#include <string>

namespace primal::tools {

enum class Severity : u8 {
    Info    = 0,
    Warning = 1,
    Error   = 2,
};

struct ErrorReport {
    Severity            severity{Severity::Warning};
    std::string         code;             // e.g. "repair.hole_fill_failed"
    std::string         message;          // human-readable detail
    std::string         source_module;    // "repair" / "remesh" / "uvatlas" / ...
    u32                 mesh_index{u32_invalid_id};

    ErrorReport() = default;
    ErrorReport(Severity s, std::string c, std::string m, std::string mod,
                u32 mesh_idx = u32_invalid_id)
        : severity{s}, code{std::move(c)}, message{std::move(m)},
          source_module{std::move(mod)}, mesh_index{mesh_idx} {}
};

}  // namespace primal::tools
