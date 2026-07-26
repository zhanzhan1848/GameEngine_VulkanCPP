// Engine/Graphics/WFC/WFCConfig.cpp
//
// Reflection descriptors for WFCConfig. Mirrors the PCGParamDescriptor pattern
// from PCGReflection.h. Descriptors are built from a static table so the
// editor / UI layer can enumerate WFC parameters by name without touching
// the struct layout directly.
//
// IMPORTANT: PCGParamRange is a struct {f32 min_val, max_val, step}. Brace-init
// with three values keeps step explicit and matches the slider semantics used
// by the editor. The order of fields in PCGParamDescriptor is:
//   { name, group, type, range, offset, size, enum_names }.

#include "WFCConfig.h"

namespace primal::graphics::wfc {

u32 WFCConfig::GetParamDescriptors(pcg::PCGParamDescriptor* out, u32 max_count) {
    // Static-lifetime strings — pointer-stable for editor hot-reload.
    static const char* kGroup_Solver = "Solver";
    static const char* kGroup_Grid   = "Grid";
    static const char* kGroup_Render = "Render";

    // Comma-separated enum labels for Mode (matches PCG convention).
    static const char* kEnumNames_Mode = "Independent2D,Independent3D,Layered,SingleDomain";

    static const pcg::PCGParamDescriptor descs[] = {
        // ---- Grid ----
        { "grid_size.x", kGroup_Grid, pcg::PCGParamType::UInt,
          { 4.0f, 256.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, grid_size.x), sizeof(u32),
          nullptr },
        { "grid_size.y", kGroup_Grid, pcg::PCGParamType::UInt,
          { 4.0f, 256.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, grid_size.y), sizeof(u32),
          nullptr },
        { "grid_size.z", kGroup_Grid, pcg::PCGParamType::UInt,
          { 4.0f, 256.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, grid_size.z), sizeof(u32),
          nullptr },

        // ---- Solver ----
        { "mode", kGroup_Solver, pcg::PCGParamType::Enum,
          { 0.0f, 3.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, mode), sizeof(WFCConfig::Mode),
          kEnumNames_Mode },
        { "seed", kGroup_Solver, pcg::PCGParamType::UInt,
          { 0.0f, 4294967295.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, seed), sizeof(u32),
          nullptr },

        // ---- Render / framing ----
        { "max_cells_per_frame", kGroup_Render, pcg::PCGParamType::UInt,
          { 1.0f, 1024.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, max_cells_per_frame), sizeof(u32),
          nullptr },
        { "max_ms_per_frame", kGroup_Render, pcg::PCGParamType::UInt,
          { 1.0f, 33.0f, 1.0f },
          WFC_OFFSETOF(WFCConfig, max_ms_per_frame), sizeof(u32),
          nullptr },
    };
    constexpr u32 kCount = static_cast<u32>(sizeof(descs) / sizeof(descs[0]));

    const u32 to_copy = (max_count < kCount) ? max_count : kCount;
    for (u32 i = 0; i < to_copy; ++i) {
        out[i] = descs[i];
    }
    return kCount;
}

} // namespace primal::graphics::wfc
