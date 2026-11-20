#include "InstanceDataODRProbe.h"
#include "Graphics/Material/InstanceData.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include <cstdint>
#include <vector>

InstanceDataODRProbeResult ProbeForwardThenSnapshot() {
    std::vector<primal::graphics::ForwardInstanceData> forward(3);
    std::vector<primal::graphics::InstanceData> snapshot(3);
    forward[2].roughness = 0.25f;
    snapshot[2].geometry_id = 37;
    return {
        forward.size(),
        snapshot.size(),
        static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(&forward[1]) -
                                 reinterpret_cast<const std::uint8_t*>(&forward[0])),
        static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(&snapshot[1]) -
                                 reinterpret_cast<const std::uint8_t*>(&snapshot[0]))};
}
