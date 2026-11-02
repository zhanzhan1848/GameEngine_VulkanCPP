#pragma once
#include "ExperimentalSAHState.h"

// The view type is supplied by Surface Cache. Keep this comparison separate so
// stale and partially published frame slots can be exercised without a GPU.
namespace primal::graphics::experimental_sah {
template<class View, class Digest>
bool WitnessStillCurrent(const View& expected, const View& current, Digest digest) {
    if (!digest || !expected.ready || !current.ready || expected.source_digest!=digest ||
        current.source_digest!=digest || current.scene_epoch!=expected.scene_epoch ||
        current.slot_epoch!=expected.slot_epoch || current.slot!=expected.slot) return false;
    for (unsigned i=0;i<4;++i)
        if(current.buffers[i]!=expected.buffers[i] || current.sizes[i]!=expected.sizes[i] || !current.sizes[i])return false;
    return true;
}
}
