#include "../../Engine/Graphics/Utilities/ExperimentalBinnedSAH.h"
#include <cassert>
#include <cstdio>
#include <random>

using namespace primal::experimental_bvh;

static bool contains(const Bounds& outer, const Bounds& inner) {
    for (int a = 0; a < 3; ++a)
        if (outer.lo[a] > inner.lo[a] || outer.hi[a] < inner.hi[a]) return false;
    return true;
}
static void verify(const std::vector<Bounds>& input) {
    Tree tree, again;
    assert(build_binned_sah(input, tree));
    assert(build_binned_sah(input, again));
    assert(tree.primitives == again.primitives);
    assert(tree.nodes.size() == again.nodes.size());
    std::vector<unsigned> seen(input.size()), nodes(tree.nodes.size());
    if (!input.empty()) {
        std::vector<std::pair<uint32_t, unsigned>> stack{{0, 1}};
        while (!stack.empty()) {
            auto [index, depth] = stack.back(); stack.pop_back();
            assert(index < tree.nodes.size() && depth <= 63 && !nodes[index]++);
            const auto& n = tree.nodes[index];
            const auto& other = again.nodes[index];
            assert(n.left == other.left && n.right == other.right &&
                   n.first == other.first && n.count == other.count);
            if (n.count) {
                assert(n.count <= 2 && n.first + n.count <= input.size());
                for (uint32_t j = n.first; j < n.first + n.count; ++j) {
                    uint32_t id = tree.primitives[j];
                    assert(id < input.size() && !seen[id]++ && contains(n.bounds, input[id]));
                }
            } else {
                assert(n.left < tree.nodes.size() && n.right < tree.nodes.size());
                assert(contains(n.bounds, tree.nodes[n.left].bounds));
                assert(contains(n.bounds, tree.nodes[n.right].bounds));
                stack.push_back({n.left, depth+1}); stack.push_back({n.right, depth+1});
            }
        }
    }
    for (auto n : seen) assert(n == 1);
    for (auto n : nodes) assert(n == 1);
}
int main() {
    verify({});
    Bounds same{{0,0,0}, {1,1,1}};
    verify(std::vector<Bounds>(257, same));
    std::mt19937 rng(20261028);
    std::uniform_real_distribution<float> position(-1000, 1000), size(0, 30);
    for (int test = 0; test != 80; ++test) {
        std::vector<Bounds> input;
        for (int i = 0; i < 1 + test * 7; ++i) {
            Bounds b;
            for (int a = 0; a < 3; ++a) {
                b.lo[a] = position(rng); b.hi[a] = b.lo[a] + size(rng);
            }
            input.push_back(b);
        }
        verify(input);
    }
    Tree result;
    Bounds bad = same; bad.lo[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!build_binned_sah({bad}, result) && result.nodes.empty());
    bad = same; bad.lo[1] = 2;
    assert(!build_binned_sah({bad}, result) && result.nodes.empty());
    std::puts("Experimental binned SAH structural checks passed");
}
