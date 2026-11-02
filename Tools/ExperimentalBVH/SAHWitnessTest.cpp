#include "../../Engine/Graphics/RenderPipeline/Modules/ExperimentalSAHWitness.h"
#include "../../Engine/Graphics/RenderPipeline/Modules/ExperimentalSAHState.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
struct View { uint64_t buffers[4]{1,2,3,4}, sizes[4]{48,48,48,64}, scene_epoch=9, slot_epoch=9, source_digest=123; unsigned slot=1; bool ready=true; };
int main() {
    using primal::graphics::experimental_sah::WitnessStillCurrent;
    View baseline, changed=baseline;
    assert(WitnessStillCurrent(baseline,changed,uint64_t(123)));
    assert(!WitnessStillCurrent(baseline,changed,uint64_t(0)));
    changed.ready=false; assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.scene_epoch;assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.slot_epoch;assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.slot;assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.source_digest;assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    for(unsigned i=0;i<4;++i) {
        ++changed.buffers[i];assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
        changed.sizes[i]=0;assert(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    }
    uint32_t policy[]{1u,2u,123u,456u};
    primal::graphics::experimental_sah::InvalidateProvenance(policy);
    assert(policy[0]==0u && policy[1]==0x80000002u && policy[2]==123u && policy[3]==456u);
    primal::graphics::experimental_sah::FailureMemo memo;
    assert(!memo.Blocks(123,9));memo.Record(123,9);
    assert(memo.Blocks(123,9) && !memo.Blocks(123,10) && !memo.Blocks(124,9));
    // Success of a new identity resets failure; the prior pair can then retry.
    memo.Reset();assert(!memo.Blocks(123,9));
    primal::graphics::rhi::FrameSlotCompletion completions;
    uint64_t consumed[3]{};
    using primal::graphics::experimental_sah::TryConsumeSlot;
    assert(!TryConsumeSlot(completions,0,consumed));
    assert(completions.Publish(0) && TryConsumeSlot(completions,0,consumed));
    assert(!TryConsumeSlot(completions,0,consumed));
    assert(!TryConsumeSlot(completions,1,consumed));
    completions.Invalidate();assert(!TryConsumeSlot(completions,0,consumed));
    assert(completions.Publish(1) && TryConsumeSlot(completions,1,consumed));
    assert(!TryConsumeSlot(completions,0,consumed));
    assert(completions.Publish(0) && TryConsumeSlot(completions,0,consumed));
    assert(!TryConsumeSlot(completions,0,consumed));
    assert(!completions.Publish(3) && !TryConsumeSlot(completions,3,consumed));
    std::puts("SAH scene witness lifecycle checks passed");
}
