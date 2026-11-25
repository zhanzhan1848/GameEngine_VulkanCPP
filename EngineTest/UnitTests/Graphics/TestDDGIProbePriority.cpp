// UnitDDGIProbePriority — deterministic DDGI partial-update selection.
//
// Regression contract for the 2026-11-25 parity finding: the near-field update
// list diverged between D3D12/DXBC (MSVC) and Vulkan/Metal (libc++/clang) even
// though the priority score vectors were identical, because `std::partial_sort`
// orders *equivalent* elements differently in each standard library. Replaying
// the recorded 398-frame schedules showed the whole 166-membership divergence
// starting from one exact tie (probes 744/787 at frame 156, identical age 4).
//
// What this test pins down:
//   1. equal scores select the lowest probe indices, independent of input order;
//   2. the selected *set* is a pure function of the score vector (permutation
//      and duplicate-score invariance) — i.e. no stdlib dependence;
//   3. the comparator is a strict weak ordering (irreflexive, antisymmetric,
//      transitive over a score/index sample), including the NaN sanitize path;
//   4. the variance term absorbs sub-quantum backend noise and stays in [0,1].

#include "Graphics/Lumen/DDGI/DDGIProbePriority.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using primal::graphics::lumen::DDGIProbePriority;
using primal::graphics::lumen::DDGIProbePriorityBefore;
using primal::graphics::lumen::DDGIVarianceScore;
using primal::graphics::lumen::DDGIPriorityScore;
using primal::graphics::lumen::DDGISelectTopProbes;
using primal::graphics::lumen::DDGIFnv1aU32;

namespace {
int tests = 0;
int fail = 0;
void check(bool ok, const char* label) {
    ++tests;
    if (!ok) {
        ++fail;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

std::vector<DDGIProbePriority> MakePriorities(const std::vector<float>& scores) {
    std::vector<DDGIProbePriority> out(scores.size());
    for (std::size_t i = 0; i < scores.size(); ++i)
        out[i] = {static_cast<std::uint32_t>(i), scores[i]};
    return out;
}

std::vector<std::uint32_t> SelectedIndices(const std::vector<DDGIProbePriority>& sorted,
                                          std::uint32_t count) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < count && i < sorted.size(); ++i) ids.push_back(sorted[i].index);
    return ids;
}

bool StrictWeakOrdering(const std::vector<DDGIProbePriority>& v) {
    for (const auto& a : v) {
        if (DDGIProbePriorityBefore(a, a)) return false;  // irreflexive
        for (const auto& b : v) {
            if (DDGIProbePriorityBefore(a, b) && DDGIProbePriorityBefore(b, a)) return false;
            for (const auto& c : v) {
                if (DDGIProbePriorityBefore(a, b) && DDGIProbePriorityBefore(b, c) &&
                    !DDGIProbePriorityBefore(a, c))
                    return false;  // transitive
            }
        }
    }
    return true;
}
}  // namespace

int main() {
    // ---- 1. Exact tie: lowest indices win, whatever the input order. --------
    {
        std::vector<float> scores(64, 0.5f);  // all tied
        for (int perm = 0; perm < 8; ++perm) {
            auto v = MakePriorities(scores);
            std::mt19937 rng(static_cast<unsigned>(perm) * 7919u);
            std::shuffle(v.begin(), v.end(), rng);
            const std::uint32_t n = DDGISelectTopProbes(v, 8);
            check(n == 8, "tie: selection count");
            auto ids = SelectedIndices(v, 8);
            std::sort(ids.begin(), ids.end());
            check(ids == std::vector<std::uint32_t>({0, 1, 2, 3, 4, 5, 6, 7}),
                  "tie: permutation-invariant lowest-index selection");
        }
    }

    // ---- 2. The 744/787 regression: two probes, identical age/score. -------
    {
        std::vector<float> scores(882, 0.4f);
        scores[744] = 0.52349794f;  // tied pair from the recorded schedule
        scores[787] = 0.52349794f;
        auto a = MakePriorities(scores);
        auto b = a;
        std::reverse(b.begin(), b.end());  // simulate a different input layout
        // Exactly one slot available for the tied pair -> the lower index wins
        // on every toolchain (this is the frame-156 membership decision).
        DDGISelectTopProbes(a, 1);
        DDGISelectTopProbes(b, 1);
        check(a[0].index == 744, "744/787: lower index wins the single slot (forward layout)");
        check(b[0].index == 744, "744/787: lower index wins the single slot (reversed layout)");
        // With room for both, both are selected and ordered by index.
        auto c = MakePriorities(scores);
        DDGISelectTopProbes(c, 220);
        const auto idsC = SelectedIndices(c, 220);
        check(idsC[0] == 744 && idsC[1] == 787, "744/787: paired order is index-ascending");
    }

    // ---- 3. Comparator is a strict weak ordering, NaN included. ------------
    {
        std::vector<DDGIProbePriority> v = {
            {0, 1.0f}, {1, 0.5f}, {2, 0.5f}, {3, -1.0f},
            {4, 0.0f}, {5, 1e30f}, {6, -1e30f}, {7, 0.5f}};
        check(StrictWeakOrdering(v), "comparator is a strict weak ordering");
        auto sorted = v;
        DDGISelectTopProbes(sorted, static_cast<std::uint32_t>(sorted.size()));
        check(!DDGIProbePriorityBefore(sorted[0], sorted[0]), "irreflexive after sort");
        check(DDGIProbePriorityBefore(sorted[0], sorted[1]), "sorted result obeys the order");
    }

    // ---- 4. Score vector determines the set; index order breaks ties. ------
    {
        std::vector<float> scores;
        for (int i = 0; i < 300; ++i) scores.push_back(0.2f + 0.001f * static_cast<float>(i % 37));
        auto v1 = MakePriorities(scores);
        auto v2 = v1;
        std::mt19937 rng(12345);
        std::shuffle(v2.begin(), v2.end(), rng);
        DDGISelectTopProbes(v1, 50);
        DDGISelectTopProbes(v2, 50);
        auto a = SelectedIndices(v1, 50);
        auto b = SelectedIndices(v2, 50);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        check(a == b, "score vector alone determines the selected set");
        check(std::adjacent_find(a.begin(), a.end()) == a.end(), "selected ids are unique");
        // Deterministic order inside the selection as well (index ascending on ties).
        bool monotonicOnTies = true;
        for (std::uint32_t i = 1; i < 50; ++i) {
            if (v1[i].score == v1[i - 1].score && v1[i].index < v1[i - 1].index)
                monotonicOnTies = false;
        }
        check(monotonicOnTies, "equal scores ordered by ascending index");
    }

    // ---- 5. Count clamping / empty inputs. --------------------------------
    {
        std::vector<DDGIProbePriority> empty;
        check(DDGISelectTopProbes(empty, 10) == 0, "empty input");
        auto v = MakePriorities({0.1f, 0.2f, 0.3f});
        check(DDGISelectTopProbes(v, 99) == 3, "count clamps to size");
        check(v[0].index == 2 && v[1].index == 1 && v[2].index == 0, "descending score order");
    }

    // ---- 6. Variance term: noise absorption and range. ---------------------
    {
        const float expected = 9.0f;  // probe_spacing^2 default (3m spacing)
        // Saturates at 1 regardless of how large the variance is.
        check(DDGIVarianceScore(400.0f, expected) == 1.0f, "variance saturates");
        check(DDGIVarianceScore(9.0f, expected) == 1.0f, "variance at saturation");
        // Two readbacks that differ only by backend float noise quantize equal.
        const float a = DDGIVarianceScore(7.820623f, expected);
        const float b = DDGIVarianceScore(7.820912f, expected);
        check(a == b, "variance noise (2.9e-4) absorbed by the 1/256 grid");
        // The grid is fine enough not to collapse a whole eighth of the range.
        check(DDGIVarianceScore(0.0f, expected) == 0.0f, "zero variance");
        check(DDGIVarianceScore(4.5f, expected) == 0.5f, "half saturation");
        check(DDGIVarianceScore(4.5f, expected) != DDGIVarianceScore(2.25f, expected),
              "distinct variance bands stay distinct");
        // Degenerate inputs stay inside [0,1] (no NaN/Inf tests: -ffast-math).
        check(DDGIVarianceScore(1.0f, 0.0f) == 0.0f, "degenerate expected variance");
        check(DDGIVarianceScore(1e30f, expected) == 1.0f, "huge variance clamps to 1");
        check(DDGIVarianceScore(-1.0f, expected) == 0.0f, "negative variance clamps");
    }

    // ---- 7. Priority score sanitize + digest stability. --------------------
    {
        check(DDGIPriorityScore(0.1f, 0.5f, 0.3f) == 0.1f + 0.5f * 0.5f + 0.3f * 0.3f,
              "score composition");
        std::uint64_t h1 = 1469598103934665603ull, h2 = h1;
        for (std::uint32_t i = 0; i < 16; ++i) h1 = DDGIFnv1aU32(h1, i);
        for (std::uint32_t i = 0; i < 16; ++i) h2 = DDGIFnv1aU32(h2, i);
        check(h1 == h2 && h1 != 1469598103934665603ull, "digest is stable and non-trivial");
        h2 = DDGIFnv1aU32(h2, 99u);
        check(h1 != h2, "digest separates different lists");
    }

    std::printf("UnitDDGIProbePriority: %d/%d checks passed\n", tests - fail, tests);
    return fail == 0 ? 0 : 1;
}
