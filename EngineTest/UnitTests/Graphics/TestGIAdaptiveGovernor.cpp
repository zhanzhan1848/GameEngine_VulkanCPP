#include "Graphics/RenderPipeline/Modules/GIAdaptiveGovernor.h"

#include <chrono>
#include <cstdio>

int main() {
    using namespace std::chrono_literals;
    primal::graphics::GIAdaptiveGovernor governor;
    std::chrono::steady_clock::time_point now{};
    governor.RecordFrame(now);
    const auto frames = [&](int count, std::chrono::milliseconds interval) {
        for (int i = 0; i < count; ++i) governor.RecordFrame(now += interval);
    };
    frames(1, 10s);
    frames(140, 16ms);
    if (governor.Scale() != 1.0f) {
        std::puts("A single pause lowered the GI budget");
        return 1;
    }
    frames(140, 1500ms);
    if (governor.ScaleBudget(882, governor.Scale()) != 220) {
        std::puts("Sustained slow frames failed to reduce the near-probe budget");
        return 1;
    }
    frames(200, 16ms);
    return governor.Scale() == 1.0f ? 0 : 1;
}
