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

#include "PrimitiveTypes.h"
#include <algorithm>
#include <chrono>
#include <optional>

namespace primal::graphics {

class GIAdaptiveGovernor {
public:
    // Record one frame; call once per rendered frame (graph build entry).
    void RecordFrame(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        if (!last_frame_) {
            last_frame_ = now;
            return;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(now - *last_frame_).count();
        last_frame_ = now;
        if (elapsedMs <= 0.0) return;
        // Bound the effect of a single pause on the EMA. Sustained slow frames
        // must still count toward the dwell window, including frames over 1 s.
        const double dtMs = std::min(elapsedMs, 100.0);

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
    double EmaMs() const { return ema_ms_; }

    // Budget helpers — scale a base count, always ≥ 1 so the caches tick.
    static u32 ScaleBudget(u32 base, f32 scale) {
        if (base == 0) return 0;   // 0 = feature off (e.g. full-update path)
        const u32 scaled = static_cast<u32>(static_cast<f32>(base) * scale);
        return scaled > 0 ? scaled : 1u;
    }

private:
    static constexpr double kAlpha      = 0.1;    // EMA smoothing
    static constexpr double kOverMs     = 33.4;   // EMA above this = below 30fps
    static constexpr double kUnderMs    = 20.0;   // EMA below this = above 50fps
    static constexpr u32 kDwellFrames = 60;    // frames sustained before a tier switch
    static constexpr f32 kMinScale   = 0.25f;

    std::optional<std::chrono::steady_clock::time_point> last_frame_;
    double ema_ms_ = 16.7;
    u32 over_frames_ = 0;
    u32 under_frames_ = 0;
    f32 scale_ = 1.0f;
};

} // namespace primal::graphics
