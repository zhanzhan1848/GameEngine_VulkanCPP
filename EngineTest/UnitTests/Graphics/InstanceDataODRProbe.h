#pragma once
#include <cstddef>

struct InstanceDataODRProbeResult {
    std::size_t forward_count;
    std::size_t snapshot_count;
    std::size_t forward_stride;
    std::size_t snapshot_stride;
};

InstanceDataODRProbeResult ProbeForwardThenSnapshot();
InstanceDataODRProbeResult ProbeSnapshotThenForward();
