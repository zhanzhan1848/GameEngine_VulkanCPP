#pragma once
namespace primal::graphics {
// The cached shader includes QUERY_KERNEL. Both initialization and runtime
// setters must retain that explicitly requested, successfully created baseline.
constexpr bool InstanceNormalCacheQueryReady(bool queryRequested,bool queryPipelineReady) {
    return queryRequested && queryPipelineReady;
}
}
