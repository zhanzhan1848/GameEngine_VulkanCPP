#include "Graphics/Lumen/ScreenProbes/ScreenProbeCacheValidation.h"
#include <iostream>
#include <limits>

using namespace primal::graphics;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::cerr << name << '\n'; ++failures; }
    };
    lumen::SurfaceCacheView sc;
    check(!lumen::IsScreenProbeSurfaceCacheValid(sc), "empty SC");
    sc.lighting_atlas = 1; sc.card_data = 2; sc.card_lookup = 3;
    sc.grid_offsets = 4; sc.grid_cards = 5; sc.atlas_size = 2048; sc.max_card_count = 4096;
    check(lumen::IsScreenProbeSurfaceCacheValid(sc), "complete SC");
    for (auto member : {&lumen::SurfaceCacheView::lighting_atlas, &lumen::SurfaceCacheView::card_data,
                        &lumen::SurfaceCacheView::card_lookup, &lumen::SurfaceCacheView::grid_offsets,
                        &lumen::SurfaceCacheView::grid_cards}) {
        auto missing = sc; missing.*member = rhi::handles::INVALID_RESOURCE;
        check(!lumen::IsScreenProbeSurfaceCacheValid(missing), "missing SC resource");
    }
    auto badSC = sc; badSC.atlas_size = 0;
    check(!lumen::IsScreenProbeSurfaceCacheValid(badSC), "zero atlas");
    badSC = sc; badSC.max_card_count = 0;
    check(!lumen::IsScreenProbeSurfaceCacheValid(badSC), "zero card capacity");
    lumen::DDGIVolumeView ddgi;
    check(!lumen::IsScreenProbeDDGIValid(ddgi), "empty DDGI");
    ddgi.irradiance = 1; ddgi.counts = {16, 8, 16}; ddgi.spacing = 2;
    check(lumen::IsScreenProbeDDGIValid(ddgi), "valid DDGI");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float value : {0.f, -1.f, 0.5f, nan, inf, 1e20f}) {
        for (int axis = 0; axis < 3; ++axis) {
            auto bad = ddgi;
            if (axis == 0) bad.counts.x = value;
            if (axis == 1) bad.counts.y = value;
            if (axis == 2) bad.counts.z = value;
            check(!lumen::IsScreenProbeDDGIValid(bad), "invalid DDGI counts");
        }
    }
    for (float value : {0.f, -1.f, nan, inf}) {
        auto bad = ddgi; bad.spacing = value;
        check(!lumen::IsScreenProbeDDGIValid(bad), "invalid DDGI spacing");
    }
    auto bad = ddgi; bad.origin.z = nan;
    check(!lumen::IsScreenProbeDDGIValid(bad), "invalid DDGI origin");
    bad = ddgi; bad.counts = {1024, 1024, 1024};
    check(!lumen::IsScreenProbeDDGIValid(bad), "SH index overflow");
    return failures ? 1 : 0;
}
