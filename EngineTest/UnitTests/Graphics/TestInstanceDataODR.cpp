#include "InstanceDataODRProbe.h"
#include "Graphics/Material/InstanceData.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include <cstddef>
#include <iostream>

static_assert(sizeof(primal::graphics::ForwardInstanceData) == 96);
static_assert(sizeof(primal::graphics::InstanceData) == 192);
static_assert(offsetof(primal::graphics::ForwardInstanceData, base_color) == 64);
static_assert(offsetof(primal::graphics::InstanceData, geometry_id) == 128);

int main() {
    const auto a = ProbeForwardThenSnapshot();
    const auto b = ProbeSnapshotThenForward();
    const bool ok = a.forward_count == 3 && a.snapshot_count == 3 &&
                    a.forward_stride == sizeof(primal::graphics::ForwardInstanceData) &&
                    a.snapshot_stride == sizeof(primal::graphics::InstanceData) &&
                    b.forward_count == 4 && b.snapshot_count == 4 &&
                    b.forward_stride == sizeof(primal::graphics::ForwardInstanceData) &&
                    b.snapshot_stride == sizeof(primal::graphics::InstanceData);
    if (!ok) {
        std::cerr << "InstanceData ODR regression: counts/strides don't match their GPU layouts\n";
        return 1;
    }
    std::cout << "InstanceData ODR regression passed\n";
    return 0;
}
