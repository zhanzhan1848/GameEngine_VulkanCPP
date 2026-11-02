#pragma once
#include <cstdint>
#include "../../RHI/Core/FrameSlotCompletion.h"

namespace primal::graphics::experimental_sah {
// Preserve existing policy bits while rejecting receiver reconstruction in
// both trace and resolve. Resolve must not borrow a previously valid half map.
inline void InvalidateProvenance(uint32_t* policy) {
    policy[0]=0u;
    policy[1]|=0x80000000u;
}
struct FailureMemo {
    uint64_t digest=0, scene_epoch=0;
    bool failed=false;
    bool Blocks(uint64_t source, uint64_t epoch) const {
        return failed && digest==source && scene_epoch==epoch;
    }
    void Record(uint64_t source, uint64_t epoch) { digest=source;scene_epoch=epoch;failed=true; }
    void Reset() { digest=scene_epoch=0;failed=false; }
};
// A fence serial is consumable once per slot; frame number is not proof.
inline bool TryConsumeSlot(const rhi::FrameSlotCompletion& completions, uint32_t slot,
                           uint64_t (&last_consumed)[3]) {
    if(slot>=3 || !completions.CanReuse(slot,last_consumed[slot]))return false;
    last_consumed[slot]=completions.Serial(slot);
    return true;
}
}
