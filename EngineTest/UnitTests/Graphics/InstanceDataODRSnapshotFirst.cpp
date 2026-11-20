#include "InstanceDataODRProbe.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/Material/InstanceData.h"
#include <cstdint>
#include <vector>

InstanceDataODRProbeResult ProbeSnapshotThenForward() {
    std::vector<primal::graphics::InstanceData> snapshot(4);
    std::vector<primal::graphics::ForwardInstanceData> forward(4);
    snapshot[3].geometry_id = 91;
    forward[3].roughness = 0.75f;
    return {
        forward.size(),
        snapshot.size(),
        static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(&forward[1]) -
                                 reinterpret_cast<const std::uint8_t*>(&forward[0])),
        static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(&snapshot[1]) -
                                 reinterpret_cast<const std::uint8_t*>(&snapshot[0]))};
}
