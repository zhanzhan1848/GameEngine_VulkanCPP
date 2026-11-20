#include "DawnSponzaShadowFit.h"
#include "Engine/Common/CoreDiag.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace primal::tests::dawn_sponza;

static void Require(bool condition, const char* message) {
    if (!condition) {
        primal::diag::Emit(primal::diag::kLevelError, "DawnShadowFit", message);
        std::exit(1);
    }
}

int main() {
    const std::string root = "Testing/RenderingBaselines/dawn_shadow_fit_20261119/";
    std::ifstream input(root + "bounds.f32", std::ios::binary);
    Require(bool(input), "Missing captured Sponza bounds");
    std::vector<ShadowBounds> bounds;
    std::array<float, 4> packed{};
    while (input.read(reinterpret_cast<char*>(packed.data()), sizeof(packed))) {
        bounds.push_back({{packed[0], packed[1], packed[2]}, packed[3]});
    }
    Require(input.eof() && input.gcount() == 0 && bounds.size() == 393,
            "Invalid captured bounds size");
    std::array<float, 64> expected{};
    std::ifstream reference(root + "matrices.f32", std::ios::binary);
    Require(bool(reference.read(reinterpret_cast<char*>(expected.data()), sizeof(expected))) &&
            reference.peek() == std::char_traits<char>::eof(), "Invalid frozen matrix size");

    ShadowParameters params{};
    const auto direction = smath::Normalize(smath::v3{.5f, -.7f, .3f});
    params.lightDirection = {direction.x, direction.y, direction.z, 0};
    FitShadowCascades(params, bounds, 2048);
    for (unsigned cascade = 0; cascade < 4; ++cascade) {
        for (unsigned column = 0; column < 4; ++column) {
            for (unsigned row = 0; row < 4; ++row) {
                const unsigned index = cascade * 16 + column * 4 + row;
                const float actual = primal::math::elem(params.lightVP[cascade], column, row);
                if (std::bit_cast<unsigned>(actual) != std::bit_cast<unsigned>(expected[index])) {
                    char message[160];
                    std::snprintf(message, sizeof(message),
                        "Frozen cascade matrix component %u: actual=%.9g expected=%.9g",
                        index, actual, expected[index]);
                    Require(false, message);
                }
            }
        }
    }
    std::puts("{\"passed\":true,\"bounds\":393,\"matrix_components\":64,\"gpu_required\":false}");
}
