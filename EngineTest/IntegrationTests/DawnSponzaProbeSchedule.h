#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace primal::tests::dawn_sponza {
// Host-side selection for the frozen near-volume policy. Grid positions are
// UNRELOCATED: relocation belongs to ray generation, not distance priority.
// GPU depth must arrive through a completed readback, never Map(storage).
class ProbeSchedule {
public:
    using Index = std::uint32_t;
    struct Grid { std::array<Index, 3> counts{}; std::array<float, 3> origin{}; float spacing{}; };
    struct DepthHistory {
        std::span<const float> values; // per probe: 64 means, then 64 variances
        Index sourceFrame = ~Index(0);
        bool completed = false;
    };
    struct Plan {
        Index frame = ~Index(0);
        std::vector<Index> indices;
        std::vector<float> scores;
        bool first = false;
    };
    bool Initialize(const Grid& grid) {
        if (ready_ || !std::isfinite(grid.spacing) || grid.spacing <= 0) return false;
        std::uint64_t count = 1;
        for (Index n : grid.counts) {
            if (!n || count > std::numeric_limits<Index>::max() / n) return false;
            count *= n;
        }
        for (float v : grid.origin) if (!std::isfinite(v)) return false;
        grid_ = grid; ages_.assign(count, 0); variances_.assign(count, 0);
        ready_ = true; return true;
    }
    // Preparation is transactional. A rejected frame/history must not alter
    // age or variance state. Commit follows successful encoding of its list.
    bool Prepare(Index frame, Index volumeFrame, Index giFrame, Index budget,
                 Index warmup, const std::array<float, 3>& camera,
                 const DepthHistory& history, Plan& result, bool cappedAge = false) {
        if (!ready_ || pending_ || frame != nextFrame_ || !budget) return false;
        for (float v : camera) if (!std::isfinite(v)) return false;
        auto variance = variances_;
        // Frozen ring becomes readable after three recorded copies. A stale,
        // future or uncompleted payload is an error, not an implicit fallback.
        if (!history.values.empty()) {
            if (frame < 3 || volumeFrame < 2 || !history.completed || history.sourceFrame != volumeFrame - 2u ||
                history.values.size() != ages_.size() * 128u) return false;
            for (Index i = 0; i < ages_.size(); ++i) {
                float maximum = 0;
                for (Index j = 0; j < 64; ++j) {
                    const float v = history.values[std::size_t(i) * 128u + 64u + j];
                    if (!std::isfinite(v)) return false;
                    maximum = std::max(maximum, v);
                }
                variance[i] = maximum;
            }
        }
        Plan plan; plan.frame = frame; plan.first = volumeFrame <= 1u;
        Index count = std::min(budget, Index(ages_.size()));
        if (plan.first) {
            if (giFrame < warmup) count = 0;
            plan.indices.resize(count);
            std::iota(plan.indices.begin(), plan.indices.end(), 0u);
        } else {
            // Actual frozen execution restores the budget in the priority
            // branch even inside warmup. Do not replace this with the stale
            // comment claiming all DDGI updates are frozen for 90 frames.
            struct Priority { Index index; float score; };
            std::vector<Priority> priorities(ages_.size());
            plan.scores.resize(ages_.size());
            const float maxExpectedVariance = grid_.spacing * grid_.spacing;
            for (Index i = 0; i < ages_.size(); ++i) {
                const Index ix = i % grid_.counts[0];
                const Index iy = (i / grid_.counts[0]) % grid_.counts[1];
                const Index iz = i / (grid_.counts[0] * grid_.counts[1]);
                const float x = grid_.origin[0] + float(ix) * grid_.spacing - camera[0];
                const float y = grid_.origin[1] + float(iy) * grid_.spacing - camera[1];
                const float z = grid_.origin[2] + float(iz) * grid_.spacing - camera[2];
                const float dist2 = x*x + y*y + z*z;
                const float distScore = 1.f / (1.f + dist2);
                const float varianceScore = std::min(variance[i] / maxExpectedVariance, 1.f);
                const float age = float(frame - ages_[i]);
                const float ageScore = (cappedAge ? std::min(age, 30.f) : age) / 30.f;
                const float score = distScore + varianceScore * .5f + ageScore * .3f;
                priorities[i] = {i, score}; plan.scores[i] = score;
            }
            // No extra tie breaker: matches the frozen std::partial_sort on
            // ascending initial indices, including the same-library tie order.
            std::partial_sort(priorities.begin(), priorities.begin() + count, priorities.end(),
                [](const Priority& a, const Priority& b) { return a.score > b.score; });
            for (Index i = 0; i < count; ++i) plan.indices.push_back(priorities[i].index);
        }
        pendingPlan_ = plan; pendingVariance_ = std::move(variance);
        pending_ = true; result = std::move(plan); return true;
    }
    bool Commit(Index frame) {
        if (!pending_ || pendingPlan_.frame != frame) return false;
        if (!pendingPlan_.first) for (Index i : pendingPlan_.indices) ages_[i] = frame;
        variances_ = std::move(pendingVariance_);
        pendingPlan_ = {}; pending_ = false; ++nextFrame_; return true;
    }
    void Cancel() { pendingPlan_ = {}; pendingVariance_.clear(); pending_ = false; }
    std::span<const Index> LastUpdates() const { return ages_; }
    void Reset() { *this = ProbeSchedule{}; }
private:
    Grid grid_{}; bool ready_ = false, pending_ = false; Index nextFrame_ = 0;
    std::vector<Index> ages_; std::vector<float> variances_, pendingVariance_;
    Plan pendingPlan_{};
};
}
