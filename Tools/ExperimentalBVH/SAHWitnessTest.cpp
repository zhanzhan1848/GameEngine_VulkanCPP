#include "../../Engine/Graphics/RenderPipeline/Modules/ExperimentalSAHWitness.h"
#include "../../Engine/Graphics/RenderPipeline/Modules/ExperimentalSAHState.h"
#include <cstdint>
#include <cstdio>
// R3（Windows 首编）：断言改显式检查——根 CMakeLists 对 Release 全局定义
// NDEBUG，且 Clang|GNU 侧的 -UNDEBUG 对冲不覆盖 MSVC：assert 在 Windows
// Release 被编译掉，测试空转。显式检查两端全配置生效，失败时输出定位
// 信息并以退出码 1 结束（macOS 语义不变：原 assert 经 -UNDEBUG 本就恒开）。
#include <cstdlib>
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #cond);                                \
            std::fflush(stderr);                                          \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

struct View { uint64_t buffers[4]{1,2,3,4}, sizes[4]{48,48,48,64}, scene_epoch=9, slot_epoch=9, source_digest=123; unsigned slot=1; bool ready=true; };
int main() {
    using primal::graphics::experimental_sah::WitnessStillCurrent;
    View baseline, changed=baseline;
    CHECK(WitnessStillCurrent(baseline,changed,uint64_t(123)));
    CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(0)));
    changed.ready=false; CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.scene_epoch;CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.slot_epoch;CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.slot;CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    ++changed.source_digest;CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    for(unsigned i=0;i<4;++i) {
        ++changed.buffers[i];CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
        changed.sizes[i]=0;CHECK(!WitnessStillCurrent(baseline,changed,uint64_t(123)));changed=baseline;
    }
    uint32_t policy[]{1u,2u,123u,456u};
    primal::graphics::experimental_sah::InvalidateProvenance(policy);
    CHECK(policy[0]==0u && policy[1]==0x80000002u && policy[2]==123u && policy[3]==456u);
    primal::graphics::experimental_sah::FailureMemo memo;
    CHECK(!memo.Blocks(123,9));memo.Record(123,9);
    CHECK(memo.Blocks(123,9) && !memo.Blocks(123,10) && !memo.Blocks(124,9));
    // Success of a new identity resets failure; the prior pair can then retry.
    memo.Reset();CHECK(!memo.Blocks(123,9));
    primal::graphics::rhi::FrameSlotCompletion completions;
    uint64_t consumed[3]{};
    using primal::graphics::experimental_sah::TryConsumeSlot;
    CHECK(!TryConsumeSlot(completions,0,consumed));
    CHECK(completions.Publish(0) && TryConsumeSlot(completions,0,consumed));
    CHECK(!TryConsumeSlot(completions,0,consumed));
    CHECK(!TryConsumeSlot(completions,1,consumed));
    completions.Invalidate();CHECK(!TryConsumeSlot(completions,0,consumed));
    CHECK(completions.Publish(1) && TryConsumeSlot(completions,1,consumed));
    CHECK(!TryConsumeSlot(completions,0,consumed));
    CHECK(completions.Publish(0) && TryConsumeSlot(completions,0,consumed));
    CHECK(!TryConsumeSlot(completions,0,consumed));
    CHECK(!completions.Publish(3) && !TryConsumeSlot(completions,3,consumed));
    std::puts("SAH scene witness lifecycle checks passed");
}
