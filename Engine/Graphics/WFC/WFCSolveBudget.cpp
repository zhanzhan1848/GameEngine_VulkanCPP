// Engine/Graphics/WFC/WFCSolveBudget.cpp
//
// Task 9 (Phase A.1): Frame-time budget primitive for the WFC solver.
#include "WFCSolveBudget.h"

namespace primal::graphics::wfc {

WFCSolveBudget::WFCSolveBudget(u32 max_cells_per_frame, u32 max_ms_per_frame)
    : max_cells_per_frame_(max_cells_per_frame),
      max_ms_per_frame_(max_ms_per_frame),
      cells_this_frame_(0),
      frame_start_(std::chrono::steady_clock::now()) {}

void WFCSolveBudget::Reset() {
    cells_this_frame_ = 0;
    frame_start_      = std::chrono::steady_clock::now();
}

void WFCSolveBudget::OnCellCollapsed() {
    cells_this_frame_++;
}

bool WFCSolveBudget::ShouldContinue() const {
    // Limit 1: per-frame cell-count cap.
    if (cells_this_frame_ >= max_cells_per_frame_) return false;

    // Limit 2: per-frame wall-clock cap.
    auto now        = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - frame_start_).count();
    if (static_cast<u64>(elapsed_ms) >= static_cast<u64>(max_ms_per_frame_)) {
        return false;
    }

    return true;
}

} // namespace primal::graphics::wfc
