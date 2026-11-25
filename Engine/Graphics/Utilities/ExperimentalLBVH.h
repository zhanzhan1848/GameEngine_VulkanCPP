#pragma once

// Offline experiment only; same contract as ExperimentalBinnedSAH.h (input
// bounds must conservatively enclose their primitives; output indices refer to
// the input array; empty output on failure; no GPU-equivalence claim). Both
// builders emit the generic Tree so ExperimentalSAHMesh.h repacks them into the
// 48B MeshBLAS unchanged:
//
//   build_lbvh  — Morton code (21 bits/axis, 63-bit keys) over triangle
//       centroids, LSD radix sort, then top-down splits at the highest
//       differing key bit: the classic linear BVH in its CPU form. Leaves keep
//       count<=2, matching median and SAH so traversal comparisons stay fair.
//   build_hlbvh — upper SAH, LBVH leaves: above kHlbvhSahThreshold primitives
//       a node splits with the same 12-bin SAH scan as ExperimentalBinnedSAH.h;
//       at or below the threshold the Morton-ordered range is split balanced,
//       which keeps leaves spatially coherent (that is the LBVH part).
//
// DFS emission keeps every child index greater than its parent index, the same
// invariant as both existing builders and SceneBVH's structural checks.
#include "ExperimentalBinnedSAH.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace primal::experimental_bvh {

// Above this many primitives an HLBVH node uses the binned SAH split; at or
// below it the node descends by balanced Morton-order halves. Compile-time
// constant on purpose: the experiment must stay reproducible from source.
inline constexpr uint32_t kHlbvhSahThreshold = 8;

namespace detail {

// Scene-box normalized Morton keys (21 bits per axis, interleaved to 63 bits).
// Degenerate axes (zero extent) contribute constant bits, which is harmless;
// fully identical centroids produce equal keys and the builders fall back to
// balanced splits.
inline uint64_t morton_interleave(uint32_t x, uint32_t y, uint32_t z) {
    uint64_t code = 0;
    for (int bit = 0; bit != 21; ++bit) {
        code |= uint64_t((x >> bit) & 1u) << (3 * bit);
        code |= uint64_t((y >> bit) & 1u) << (3 * bit + 1);
        code |= uint64_t((z >> bit) & 1u) << (3 * bit + 2);
    }
    return code;
}

// Fills keys[] and sorts order[] (must be iota on entry) by key, LSD radix.
inline void morton_sort(const std::vector<Bounds>& input,
                        std::vector<uint64_t>& keys, std::vector<uint32_t>& order) {
    Bounds scene = empty_bounds();
    for (const auto& b : input) {
        Bounds centroid{};
        for (int axis = 0; axis != 3; ++axis) {
            const double c = center(b, axis);
            centroid.lo[axis] = centroid.hi[axis] = float(c);
        }
        include(scene, centroid);
    }
    const double extent[3] = {
        double(scene.hi[0]) - scene.lo[0],
        double(scene.hi[1]) - scene.lo[1],
        double(scene.hi[2]) - scene.lo[2],
    };
    const uint32_t n = uint32_t(input.size());
    keys.resize(n);
    for (uint32_t i = 0; i != n; ++i) {
        const auto& b = input[i];
        uint32_t q[3];
        for (int axis = 0; axis != 3; ++axis) {
            double t = 0.0;
            if (extent[axis] > 0.0)
                t = (center(b, axis) - double(scene.lo[axis])) / extent[axis];
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            q[axis] = uint32_t(t * 2097151.0 + 0.5);  // 2^21 - 1
        }
        keys[i] = morton_interleave(q[0], q[1], q[2]);
    }
    // 8-bit LSD radix, 8 passes cover the full 64-bit key space.
    std::vector<uint64_t> scratch_keys(n);
    std::vector<uint32_t> scratch_order(n);
    std::array<uint32_t, 256> histogram{};
    for (int pass = 0; pass != 8; ++pass) {
        const int shift = 8 * pass;
        histogram.fill(0u);
        for (uint32_t i = 0; i != n; ++i) ++histogram[(keys[i] >> shift) & 0xffu];
        uint32_t total = 0;
        for (uint32_t b = 0; b != 256; ++b) {
            const uint32_t c = histogram[b];
            histogram[b] = total;
            total += c;
        }
        for (uint32_t i = 0; i != n; ++i) {
            const uint32_t dst = histogram[(keys[i] >> shift) & 0xffu]++;
            scratch_keys[dst] = keys[i];
            scratch_order[dst] = order[i];
        }
        keys.swap(scratch_keys);
        order.swap(scratch_order);
    }
}

inline bool bounds_finite(const std::vector<Bounds>& input) {
    for (const auto& b : input)
        for (int axis = 0; axis != 3; ++axis)
            if ((std::bit_cast<uint32_t>(b.lo[axis]) & 0x7f800000u) == 0x7f800000u ||
                (std::bit_cast<uint32_t>(b.hi[axis]) & 0x7f800000u) == 0x7f800000u || b.lo[axis] > b.hi[axis])
                return false;
    return true;
}

// 12-bin SAH split over primitives[first, first+count). Mirrors the scan in
// ExperimentalBinnedSAH.h detail::Builder (keep bins in lockstep); returns
// false when no valid split exists (all centroids identical on every axis).
struct SahSplit {
    int axis = -1;
    int bin = 0;
    double low = 0.0, extent = 0.0;
};
inline int sah_bin(double value, double low, double extent, int bins) {
    // Same quantization as detail::Builder::bin_for (private there).
    return std::min(bins - 1, std::max(0, int((value - low) / extent * bins)));
}
inline bool choose_sah_split(const std::vector<Bounds>& input, const std::vector<uint32_t>& primitives,
                             uint32_t first, uint32_t count, SahSplit& out) {
    static constexpr int bins = 12;
    double best = std::numeric_limits<double>::max();
    bool found = false;
    for (int axis = 0; axis != 3; ++axis) {
        double low = std::numeric_limits<double>::max();
        double high = -low;
        for (uint32_t i = first; i < first + count; ++i) {
            const double c = center(input[primitives[i]], axis);
            low = std::min(low, c);
            high = std::max(high, c);
        }
        const double extent = high - low;
        if (!(extent > 0)) continue;
        std::array<Bin, bins> b{};
        for (uint32_t i = first; i < first + count; ++i) {
            const auto& p = input[primitives[i]];
            auto& bin = b[sah_bin(center(p, axis), low, extent, bins)];
            include(bin.bounds, p);
            ++bin.count;
        }
        std::array<Bounds, bins> lb{}, rb{};
        std::array<uint32_t, bins> lc{}, rc{};
        Bounds l = empty_bounds(), r = empty_bounds();
        uint32_t nl = 0, nr = 0;
        for (int i = 0; i < bins; ++i) {
            if (b[i].count) include(l, b[i].bounds);
            nl += b[i].count;
            lb[i] = l;
            lc[i] = nl;
            const int j = bins - 1 - i;
            if (b[j].count) include(r, b[j].bounds);
            nr += b[j].count;
            rb[j] = r;
            rc[j] = nr;
        }
        for (int i = 0; i < bins - 1; ++i) {
            if (!lc[i] || !rc[i + 1]) continue;
            const double cost = area(lb[i]) * lc[i] + area(rb[i + 1]) * rc[i + 1];
            if (cost < best) {
                best = cost;
                out = SahSplit{axis, i, low, extent};
                found = true;
            }
        }
    }
    return found;
}

// Shared DFS emit skeleton: bounds over the range, leaf at count<=2, two
// children appended right after the parent (children indices > parent index).
// subtree returns false past max depth; callers fail the whole build.
template <typename Derived>
class DfsBuilder {
public:
    // Public: derived classes inherit it with this access, and callers
    // construct the derived builders directly.
    DfsBuilder(const std::vector<Bounds>& input, const std::vector<uint64_t>& keys,
               std::vector<uint32_t>& primitives, Tree& tree)
        : input_(input), keys_(keys), primitives_(primitives), tree_(tree) {}

protected:
    const std::vector<Bounds>& input_;
    const std::vector<uint64_t>& keys_;
    // Mutable on purpose: stable_partition permutes the sorted order in place.
    std::vector<uint32_t>& primitives_;
    Tree& tree_;
    static constexpr uint32_t max_depth = 63;  // 64-entry traversal stacks

    bool emit_leaf_or_fail(uint32_t node_index, uint32_t first, uint32_t count, uint32_t depth) {
        Bounds bound = empty_bounds();
        for (uint32_t i = first; i < first + count; ++i)
            include(bound, input_[primitives_[i]]);
        tree_.nodes[node_index].bounds = bound;
        if (count <= 2) {
            tree_.nodes[node_index].first = first;
            tree_.nodes[node_index].count = count;
            return true;
        }
        if (depth >= max_depth) return false;
        return derived().split_and_descend(node_index, first, count, depth);
    }
    uint32_t push_children(uint32_t node_index) {
        const uint32_t left = uint32_t(tree_.nodes.size());
        tree_.nodes.emplace_back();
        tree_.nodes.emplace_back();
        tree_.nodes[node_index].left = left;
        tree_.nodes[node_index].right = left + 1;
        return left;
    }

private:
    Derived& derived() { return static_cast<Derived&>(*this); }
};

class LbvhBuilder : public DfsBuilder<LbvhBuilder> {
public:
    using DfsBuilder::DfsBuilder;
    bool run() { return emit_leaf_or_fail(0, 0, uint32_t(input_.size()), 1); }
    bool split_and_descend(uint32_t node_index, uint32_t first, uint32_t count, uint32_t depth) {
        uint32_t left_count = count / 2;
        const uint64_t xored = keys_[first] ^ keys_[first + count - 1];
        if (xored) {
            // Sorted keys share every bit above the highest differing bit, so
            // the split at that bit is a contiguous boundary.
            const int bit = 63 - std::countl_zero(xored);
            const uint64_t mask = uint64_t(1) << bit;
            uint32_t mid = first;
            while ((keys_[mid] & mask) == 0) ++mid;
            left_count = mid - first;
        }  // else: identical centroids, balanced fallback
        if (!left_count || left_count == count) return false;
        const uint32_t left = push_children(node_index);
        return emit_leaf_or_fail(left, first, left_count, depth + 1) &&
               emit_leaf_or_fail(left + 1, first + left_count, count - left_count, depth + 1);
    }
};

class HlbvhBuilder : public DfsBuilder<HlbvhBuilder> {
public:
    using DfsBuilder::DfsBuilder;
    bool run() { return emit_leaf_or_fail(0, 0, uint32_t(input_.size()), 1); }
    bool split_and_descend(uint32_t node_index, uint32_t first, uint32_t count, uint32_t depth) {
        uint32_t left_count = 0;
        if (count > kHlbvhSahThreshold) {
            SahSplit split;
            if (choose_sah_split(input_, primitives_, first, count, split)) {
                const auto begin = primitives_.begin() + first;
                const auto middle = std::stable_partition(begin, begin + count, [&](uint32_t id) {
                    return sah_bin(center(input_[id], split.axis), split.low, split.extent, 12) <= split.bin;
                });
                left_count = uint32_t(middle - begin);
            }
            // No valid SAH split (degenerate cluster): fall through to balanced.
        }
        if (!left_count) left_count = count / 2;
        if (!left_count || left_count == count) return false;
        const uint32_t left = push_children(node_index);
        return emit_leaf_or_fail(left, first, left_count, depth + 1) &&
               emit_leaf_or_fail(left + 1, first + left_count, count - left_count, depth + 1);
    }
};

} // namespace detail

inline bool build_lbvh(const std::vector<Bounds>& input, Tree& out) {
    out = {};
    if (input.size() > std::numeric_limits<uint32_t>::max() / 2) return false;
    if (!detail::bounds_finite(input)) return false;
    if (input.empty()) return true;
    Tree candidate;
    candidate.primitives.resize(input.size());
    std::iota(candidate.primitives.begin(), candidate.primitives.end(), 0u);
    std::vector<uint64_t> keys;
    detail::morton_sort(input, keys, candidate.primitives);
    candidate.nodes.emplace_back();
    if (!detail::LbvhBuilder(input, keys, candidate.primitives, candidate).run()) return false;
    out = std::move(candidate);
    return true;
}

inline bool build_hlbvh(const std::vector<Bounds>& input, Tree& out) {
    out = {};
    if (input.size() > std::numeric_limits<uint32_t>::max() / 2) return false;
    if (!detail::bounds_finite(input)) return false;
    if (input.empty()) return true;
    Tree candidate;
    candidate.primitives.resize(input.size());
    std::iota(candidate.primitives.begin(), candidate.primitives.end(), 0u);
    std::vector<uint64_t> keys;
    detail::morton_sort(input, keys, candidate.primitives);
    candidate.nodes.emplace_back();
    if (!detail::HlbvhBuilder(input, keys, candidate.primitives, candidate).run()) return false;
    out = std::move(candidate);
    return true;
}

} // namespace primal::experimental_bvh
