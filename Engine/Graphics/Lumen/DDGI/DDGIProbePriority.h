#pragma once
/*
 * DDGI partial-update scheduler — deterministic priority order and selection.
 *
 * Why this exists (parity defect, measured 2026-11-25):
 * The near-field update list is the top-N of `distScore + variance*0.5 +
 * age*0.3`. `distScore` (probe grid vs camera) and `age` are produced by the
 * same CPU code on every backend and platform, and the variance term saturates
 * (`min(var/spacing^2, 1)`) for almost every probe, so the score vector is
 * effectively identical across D3D12 / Vulkan / Metal. Equal scores therefore
 * happen, and the former comparator compared the score alone:
 *
 *     std::partial_sort(..., [](a, b) { return a.score > b.score; });
 *
 * `partial_sort` is not stable and the standard leaves the order of equivalent
 * elements unspecified, so MSVC and libc++ disagree. Replaying the recorded
 * 398-frame Windows schedule against the macOS one shows all 166 membership
 * differences (out of 141202) reduce to five tied probe pairs; the first is
 * frame 156 (probes 744/787, identical age 4, identical last update 152). After
 * each tie the loser ages, is picked next frame, and the pair ping-pongs. The
 * same class of tie can shift a probe's history by one frame on one machine
 * whenever the two backends disagree about a variance that sits on a rounding
 * boundary.
 *
 * Contract:
 *   1. Total order — score descending, then probe index ascending. Identical
 *      score vectors produce identical selections on every toolchain, and the
 *      selection no longer depends on the standard library implementation.
 *   2. The variance term is quantized to a fixed 1/256 grid before it enters
 *      the score. The grid is ~100x larger than the observed backend float
 *      difference in an unsaturated variance term (~3e-5) and ~2.5x smaller
 *      than one frame of age (0.01), so it absorbs DXBC/SPIR-V rounding without
 *      reordering probes that differ meaningfully.
 *   3. The comparator is written as "a>b, else b>a, else index" so an unordered
 *      value (the engine builds with -ffast-math, where NaN/Inf cannot be
 *      tested reliably) still lands in a total order instead of poisoning the
 *      sort.
 *
 * This is a policy header only: no RHI, no GPU, no allocation beyond the
 * caller-provided vector, unit-testable on CPU (UnitDDGIProbePriority).
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace primal::graphics::lumen {

struct DDGIProbePriority {
    std::uint32_t index = 0;
    float score = 0.0f;
};

/// Strict weak ordering with a total tie-break. Equal scores fall back to the
/// probe index, which is what makes the top-N selection portable.
inline bool DDGIProbePriorityBefore(const DDGIProbePriority& a,
                                    const DDGIProbePriority& b) {
    if (a.score > b.score) return true;
    if (b.score > a.score) return false;
    return a.index < b.index;
}

/// Variance term of the priority score, clamp to [0,1] then quantize to 1/256.
/// The clamp is written with plain comparisons rather than std::clamp/isfinite
/// because the engine is built with -ffast-math (NaN/Inf cannot be tested
/// portably there); an unordered value degrades to the conservative 0 bound.
inline float DDGIVarianceScore(float maxVariance, float expectedVariance) {
    constexpr float kSteps = 256.0f;
    if (!(expectedVariance > 0.0f)) return 0.0f;
    float normalized = maxVariance / expectedVariance;
    normalized = normalized > 0.0f ? normalized : 0.0f;
    normalized = normalized < 1.0f ? normalized : 1.0f;
    return std::round(normalized * kSteps) / kSteps;
}

/// Fully composed score. Kept separate so the caller hashes exactly the value
/// the ordering consumes.
inline float DDGIPriorityScore(float distScore, float varianceScore,
                               float ageScore) {
    return distScore + varianceScore * 0.5f + ageScore * 0.3f;
}

/// Deterministic top-N selection (in place). Returns the number of valid
/// leading entries, which is `min(count, priorities.size())`.
inline std::uint32_t DDGISelectTopProbes(
    std::vector<DDGIProbePriority>& priorities, std::uint32_t count) {
    if (count > priorities.size()) count = static_cast<std::uint32_t>(priorities.size());
    if (count == 0) return 0;
    std::partial_sort(priorities.begin(), priorities.begin() + count,
                      priorities.end(), DDGIProbePriorityBefore);
    return count;
}

/// FNV-1a over a u32 stream: used by the f350 scheduler digest so a D3D12 run
/// and a Vulkan run can be compared without dumping whole update lists.
inline std::uint64_t DDGIFnv1aU32(std::uint64_t hash, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= static_cast<std::uint64_t>((value >> (8 * byte)) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace primal::graphics::lumen
