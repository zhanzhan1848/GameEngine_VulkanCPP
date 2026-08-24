#pragma once

// ============================================================================
// GIAdaptiveGovernor (T4.1) — frame-rate-driven quality governor.
//
// Enforces the FPS ≥ 30 floor (plan decision ②): a sliding-average frame
// time with hysteresis tiers scales the GI per-frame budgets (DDGI probe
// update count, SurfaceCache LightEval page budget). Ray counts are NOT
// modulated — they are baked into buffer allocations.
//
// Degrade: EMA frame time > kOverMs for kDwellFrames  → scale × 0.5
// Upgrade: EMA frame time < kUnderMs for kDwellFrames → scale × 2 (≤ 1.0)
// The dwell window prevents oscillation; the scale floor (0.25) keeps the
// caches alive so recovery is temporal, not a cold restart.
//
// CPU-only — safe on WASM (no readbacks involved).
// ============================================================================

#include "CommonHeaders.h"
#include <chrono>

namespace primal::graphics {

class GIAdaptiveGovernor {
public:
    // Record one frame; call once per rendered frame (graph build entry).
    void RecordFrame() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const f64 nowSec = static_cast<f64>(now) * 1e-9;
        if (last_time_sec_ < 0.0) {
            last_time_sec_ = nowSec;
            return;   // first frame establishes the baseline only
        }
        const f64 dtMs = (nowSec - last_time_sec_) * 1000.0;
        last_time_sec_ = nowSec;
        if (dtMs <= 0.0 || dtMs > 1000.0) return;   // tab-out / pause spikes

        // EMA over frame time (ms)
        ema_ms_ = ema_ms_ * (1.0 - kAlpha) + dtMs * kAlpha;

        if (ema_ms_ > kOverMs) {
            ++over_frames_;
            under_frames_ = 0;
        } else if (ema_ms_ < kUnderMs) {
            ++under_frames_;
            over_frames_ = 0;
        } else {
            // Comfort band — reset dwell counters, hold current tier.
            over_frames_ = 0;
            under_frames_ = 0;
        }

        if (over_frames_ >= kDwellFrames && scale_ > kMinScale) {
            scale_ *= 0.5f;
            over_frames_ = 0;
        } else if (under_frames_ >= kDwellFrames && scale_ < 1.0f) {
            scale_ = std::min(scale_ * 2.0f, 1.0f);
            under_frames_ = 0;
        }
    }

    f32  Scale() const { return scale_; }
    f64  EmaMs() const { return ema_ms_; }

    // Budget helpers — scale a base count, always ≥ 1 so the caches tick.
    static u32 ScaleBudget(u32 base, f32 scale) {
        if (base == 0) return 0;   // 0 = feature off (e.g. full-update path)
        const u32 scaled = static_cast<u32>(static_cast<f32>(base) * scale);
        return scaled > 0 ? scaled : 1u;
    }

private:
    static constexpr f64 kAlpha      = 0.1;    // EMA smoothing
    static constexpr f64 kOverMs     = 33.4;   // EMA above this = below 30fps
    static constexpr f64 kUnderMs    = 20.0;   // EMA below this = above 50fps
    static constexpr u32 kDwellFrames = 60;    // frames sustained before a tier switch
    static constexpr f32 kMinScale   = 0.25f;

    f64 last_time_sec_ = -1.0;
    f64 ema_ms_ = 16.7;
    u32 over_frames_ = 0;
    u32 under_frames_ = 0;
    f32 scale_ = 1.0f;
};

} // namespace primal::graphics
