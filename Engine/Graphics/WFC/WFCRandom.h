// Engine/Graphics/WFC/WFCRandom.h
//
// Task 1 (Phase A.2): Deterministic xorshift32* RNG for the WFC solver.
//
// The solver needs reproducible randomness across platforms/runs for the same
// seed (PCG must be deterministic). xorshift32* (Marsaglia) is one of the
// cheapest deterministic PRNGs with good enough statistical quality for WFC
// cell/tile selection. State is a single u32, so the struct is trivially
// copyable and small enough to pass by value if needed.
#pragma once

#include "../../Common/CommonHeaders.h"

namespace primal::graphics::wfc {

// Deterministic xorshift32* RNG. Reproducible across platforms for the same seed.
class WFCRandom {
public:
    explicit WFCRandom(u32 seed) : state_(seed == 0 ? 1u : seed) {}

    u32  NextU32();                    // full-range u32
    u32  NextRange(u32 n);             // returns [0, n)
    f32  NextF32();                    // returns [0.0, 1.0)
    bool NextBool(f32 p_true = 0.5f);  // p_true chance of true

    void Reset(u32 seed) { state_ = (seed == 0) ? 1u : seed; }

private:
    u32 state_;
};

} // namespace primal::graphics::wfc
