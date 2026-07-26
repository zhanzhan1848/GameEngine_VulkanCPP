// Engine/Graphics/WFC/WFCSolveBudget.h
//
// Task 9 (Phase A.1): Frame-time budget primitive for the WFC solver.
//
// The WFC solver can be expensive (NP-hard in the worst case). To keep the
// frame budget predictable we cap each frame's solve work by two limits:
//   * max_cells_per_frame — a hard cap on the number of cell collapses.
//   * max_ms_per_frame    — a wall-clock cap measured via std::chrono.
//
// Whichever limit trips first asks the solver to yield until next frame.
//
// Threading contract:
//   * Reset()           — main thread, frame start.
//   * OnCellCollapsed() — solver thread (single-threaded in Phase A.1; the
//                         API is designed so we can relax this later).
//   * ShouldContinue()  — solver thread, polled after each collapse.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Common/PrimitiveTypes.h"
#include <chrono>

namespace primal::graphics::wfc {

class WFCSolveBudget {
public:
    WFCSolveBudget(u32 max_cells_per_frame, u32 max_ms_per_frame);

    // Called at frame start (main thread).
    void Reset();

    // Called by the solver after each cell collapse.
    void OnCellCollapsed();

    // Returns true if the solver should continue collapsing this frame.
    bool ShouldContinue() const;

    u32 CellsThisFrame()    const { return cells_this_frame_; }
    u32 MaxCellsPerFrame()  const { return max_cells_per_frame_; }
    u32 MaxMsPerFrame()     const { return max_ms_per_frame_; }

private:
    u32                                  max_cells_per_frame_;
    u32                                  max_ms_per_frame_;
    u32                                  cells_this_frame_{0};
    std::chrono::steady_clock::time_point frame_start_;
};

} // namespace primal::graphics::wfc
