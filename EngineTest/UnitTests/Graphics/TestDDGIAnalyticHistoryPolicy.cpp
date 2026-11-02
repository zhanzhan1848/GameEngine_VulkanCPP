#include "Engine/Graphics/Lumen/DDGI/DDGIAnalyticHistoryPolicy.h"
#include <cstdio>

using namespace primal::graphics::lumen;

int main() {
    using Mode = DDGIAnalyticHistoryMode;
    struct Case { const char* disabled; const char* legacy; const char* irradiance; Mode expected; };
    const Case cases[] = {
        {nullptr, nullptr, nullptr, Mode::LambertDiffuse},
        {nullptr, nullptr, "1", Mode::LambertDiffuse},
        {nullptr, nullptr, "0", Mode::LambertDiffuse},
        {nullptr, "1", nullptr, Mode::LegacyRadiance},
        {nullptr, "1", "1", Mode::LegacyRadiance},
        {nullptr, "0", nullptr, Mode::LambertDiffuse},
        {nullptr, "", nullptr, Mode::LambertDiffuse},
        {nullptr, "01", nullptr, Mode::LambertDiffuse},
        {nullptr, "true", nullptr, Mode::LambertDiffuse},
        {"1", "1", "1", Mode::Disabled},
        {"0", "1", nullptr, Mode::Disabled}, // old presence semantics
        {"", nullptr, nullptr, Mode::Disabled}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const auto& c = cases[i];
        if (ResolveDDGIAnalyticHistoryMode(c.disabled, c.legacy, c.irradiance) != c.expected) {
            std::fprintf(stderr, "analytic history policy case %u failed\n", i);
            return 1;
        }
    }
    static_assert(static_cast<unsigned>(Mode::LegacyRadiance) == 0);
    static_assert(static_cast<unsigned>(Mode::Disabled) == 1);
    static_assert(static_cast<unsigned>(Mode::LambertDiffuse) == 2);
    static_assert(ResolveDDGIAnalyticHistoryMode() == Mode::LambertDiffuse);
    return 0;
}
