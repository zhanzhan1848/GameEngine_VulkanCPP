#pragma once

#include "CommonHeaders.h"

namespace primal::graphics::lumen {

// ============================================================================
// WRCSlotMap — [WRC] 三缓冲槽位派生。
//
// §1.2-4（刻意复制不共享）：本文件复制 DDGISlotMap.h 的语义（writeIndex /
// prev 契约）而非 include 复用。理由：两条线的时域链路（EMA 参数、reset
// 时机、失效语义）必须能独立演进，共享头会重新耦合。这是刻意的代码重复，
// 不做 DRY 重构——禁止在 WRC 消费端 include DDGISlotMap.h（§1.2-1 include
// 禁令）。
//
// 语义（frame = 绝对帧号，从 0 计）：
//   writeIndex    = frame % 3        —— 本帧唯一写入的槽
//   currentIndex  = frame % 3        —— WRC 完成帧后本帧数据所在槽
//   previousIndex = (frame + 2) % 3  —— 上一完成帧的写槽，本帧只读
//                                      （BeginFrame fence 保证 frame-3
//                                      同槽写者已完成）
// ============================================================================
struct WRCFrameResources {
    u32 writeIndex;
    u32 currentIndex;
    u32 previousIndex;
};

inline WRCFrameResources WRCSlotsForFrame(u32 frame) {
    const u32 w = frame % 3u;
    return { w, w, (frame + 2u) % 3u };
}

} // namespace primal::graphics::lumen
