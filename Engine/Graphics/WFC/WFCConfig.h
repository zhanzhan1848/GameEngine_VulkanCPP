// Engine/Graphics/WFC/WFCConfig.h
//
// Task 8 (Phase A.1): User-configurable settings struct for the WFC solver.
// Mirrors the PCGParamDescriptor reflection pattern so the editor / UI / script
// layers can read and write WFC parameters by name.
//
// Design notes:
//   * Plain struct — no methods other than the static reflection helper.
//   * Default member initializers (C++17) so `WFCConfig config;` yields the
//     documented defaults without a constructor.
//   * Explicit pad bytes (_pad_mode / _pad_render) keep the layout predictable
//     for offsetof-based reflection even though we never persist the pads.
//   * GetParamDescriptors returns descriptors for the most user-tunable
//     parameters. Phase A.2 may extend the list (organic tile count, etc.).
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"
#include "../PCG/PCGReflection.h"

namespace primal::graphics::wfc {

struct WFCConfig {
    // Solver domain shape.
    enum class Mode : u8 {
        Independent2D   = 0,
        Independent3D   = 1,
        Layered         = 2,
        SingleDomain    = 3,
    };

    // ---- Grid configuration ----
    WFCGridCoord   grid_size{16, 16, 16};   // signed s32 cells (WFCTypes.h)
    u32            grid_size_min{4};
    u32            grid_size_max{256};

    // ---- Solver mode ----
    Mode           mode{Mode::Independent3D};
    u32            seed{1337};
    u32            max_generations{8};
    bool           use_parallel_propagator{true};
    u8             _pad_mode[3];             // explicit alignment pad

    // ---- Rendering / framing ----
    u32            max_cells_per_frame{64};
    u32            max_ms_per_frame{4};
    bool           show_entropy_heatmap{true};
    bool           show_in_progress_wireframes{true};
    u8             _pad_render[2];           // explicit alignment pad

    // ---- Tiles ----
    u32            parametric_tile_count{12};
    u32            organic_tile_count{8};

    // ---- Reflection ----
    // Fills `out` with up to `max_count` descriptors and returns the total
    // descriptor count (which may exceed `max_count` if the buffer was too
    // small — caller is expected to size the buffer accordingly).
    static u32     GetParamDescriptors(pcg::PCGParamDescriptor* out, u32 max_count);
};

// offsetof helper — alias to the existing PCG macro (which itself wraps
// standard offsetof with a static_cast to u32). Defined after the struct
// body because offsetof requires a complete type.
#define WFC_OFFSETOF(T, m) PCG_OFFSETOF(T, m)

} // namespace primal::graphics::wfc
