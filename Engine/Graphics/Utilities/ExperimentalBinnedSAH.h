#pragma once

// Offline experiment only. This does not change the engine's builder or cache.
// Input bounds must conservatively enclose their primitives. Output indices
// refer to the input array; callers must version any persisted result.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace primal::experimental_bvh {
struct Bounds {
    std::array<float, 3> lo, hi;
};
struct Node {
    Bounds bounds;
    uint32_t left = 0, right = 0, first = 0, count = 0;
};
struct Tree {
    std::vector<Node> nodes;
    std::vector<uint32_t> primitives;
};

namespace detail {
inline Bounds empty_bounds() {
    const float inf = std::numeric_limits<float>::max();
    return {{inf, inf, inf}, {-inf, -inf, -inf}};
}
inline void include(Bounds& a, const Bounds& b) {
    for (int i = 0; i != 3; ++i) {
        a.lo[i] = std::min(a.lo[i], b.lo[i]);
        a.hi[i] = std::max(a.hi[i], b.hi[i]);
    }
}
inline double area(const Bounds& b) {
    const double x = double(b.hi[0]) - b.lo[0];
    const double y = double(b.hi[1]) - b.lo[1];
    const double z = double(b.hi[2]) - b.lo[2];
    return 2.0 * (x*y + y*z + z*x);
}
inline double center(const Bounds& b, int axis) {
    return (double(b.lo[axis]) + b.hi[axis]) * 0.5;
}
struct Bin { Bounds bounds = empty_bounds(); uint32_t count = 0; };

class Builder {
    const std::vector<Bounds>& input_;
    Tree& tree_;
    static constexpr int bins = 12;
    // 64 entries are used by the current GPU traversal; cap depth so a
    // pathological SAH split fails instead of silently generating overflow.
    static constexpr uint32_t max_depth = 63;
    static int bin_for(double value, double low, double extent) {
        return std::min(bins - 1, std::max(0, int((value - low) / extent * bins)));
    }
    bool build(uint32_t node_index, uint32_t first, uint32_t count, uint32_t depth) {
        Bounds bound = empty_bounds();
        for (uint32_t i = first; i < first + count; ++i)
            include(bound, input_[tree_.primitives[i]]);
        tree_.nodes[node_index].bounds = bound;
        if (count <= 2) {
            tree_.nodes[node_index].first = first;
            tree_.nodes[node_index].count = count;
            return true;
        }
        if (depth >= max_depth) return false;
        double best = std::numeric_limits<double>::max();
        int best_axis = -1, best_split = 0;
        double best_low = 0, best_extent = 0;
        for (int axis = 0; axis != 3; ++axis) {
            double low = std::numeric_limits<double>::max();
            double high = -low;
            for (uint32_t i = first; i < first + count; ++i) {
                double c = center(input_[tree_.primitives[i]], axis);
                low = std::min(low, c); high = std::max(high, c);
            }
            const double extent = high - low;
            if (!(extent > 0)) continue;
            std::array<Bin, bins> b{};
            for (uint32_t i = first; i < first + count; ++i) {
                const auto& p = input_[tree_.primitives[i]];
                auto& bin = b[bin_for(center(p, axis), low, extent)];
                include(bin.bounds, p); ++bin.count;
            }
            std::array<Bounds, bins> lb{}, rb{};
            std::array<uint32_t, bins> lc{}, rc{};
            Bounds l = empty_bounds(), r = empty_bounds();
            uint32_t nl = 0, nr = 0;
            for (int i = 0; i < bins; ++i) {
                if (b[i].count) include(l, b[i].bounds);
                nl += b[i].count; lb[i] = l; lc[i] = nl;
                const int j = bins - 1 - i;
                if (b[j].count) include(r, b[j].bounds);
                nr += b[j].count; rb[j] = r; rc[j] = nr;
            }
            for (int i = 0; i < bins - 1; ++i) {
                if (!lc[i] || !rc[i+1]) continue;
                double cost = area(lb[i]) * lc[i] + area(rb[i+1]) * rc[i+1];
                if (cost < best) {
                    best = cost; best_axis = axis; best_split = i;
                    best_low = low; best_extent = extent;
                }
            }
        }
        auto begin = tree_.primitives.begin() + first;
        auto end = begin + count;
        uint32_t left_count = count / 2;
        if (best_axis >= 0) {
            auto middle = std::stable_partition(begin, end, [&](uint32_t id) {
                return bin_for(center(input_[id], best_axis), best_low, best_extent) <= best_split;
            });
            left_count = uint32_t(middle - begin);
        }
        if (!left_count || left_count == count) return false;
        uint32_t left = uint32_t(tree_.nodes.size());
        tree_.nodes.emplace_back(); tree_.nodes.emplace_back();
        tree_.nodes[node_index].left = left;
        tree_.nodes[node_index].right = left + 1;
        return build(left, first, left_count, depth + 1) &&
               build(left + 1, first + left_count, count - left_count, depth + 1);
    }
public:
    Builder(const std::vector<Bounds>& input, Tree& tree) : input_(input), tree_(tree) {}
    bool run() { tree_.nodes.emplace_back(); return build(0, 0, uint32_t(input_.size()), 1); }
};
} // namespace detail

// Returns false on invalid bounds, an excessive tree depth, or index overflow;
// output remains empty on failure. Does not claim any GPU traversal equivalence.
inline bool build_binned_sah(const std::vector<Bounds>& input, Tree& out) {
    out = {};
    if (input.size() > std::numeric_limits<uint32_t>::max() / 2) return false;
    for (const auto& b : input)
        for (int axis = 0; axis != 3; ++axis)
            if ((std::bit_cast<uint32_t>(b.lo[axis]) & 0x7f800000u) == 0x7f800000u ||
                (std::bit_cast<uint32_t>(b.hi[axis]) & 0x7f800000u) == 0x7f800000u || b.lo[axis] > b.hi[axis])
                return false;
    if (input.empty()) return true;
    Tree candidate;
    candidate.primitives.resize(input.size());
    std::iota(candidate.primitives.begin(), candidate.primitives.end(), 0u);
    if (!detail::Builder(input, candidate).run()) return false;
    out = std::move(candidate);
    return true;
}
} // namespace primal::experimental_bvh
