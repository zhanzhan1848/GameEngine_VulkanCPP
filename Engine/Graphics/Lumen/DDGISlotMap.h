#pragma once

#include "CommonHeaders.h"

namespace primal::graphics::lumen {

// ============================================================================
// DDGISlotMap — DDGI 三缓冲槽位的单一事实源（GI 能量所有权修复 Phase 1d，
// 审计 §2.4/P0'）。
//
// 背景：全链曾裸算 frame%3，且 Fusion 三缓冲用 cbIdx（swapchain 槽）时钟
// 与 DDGI 体系的 frameCount%3 时钟无同步不变量（审计新发现①，cbIdx 错位
// 时 prev-screen 会读到本帧正在写的槽）。Phase 1d 起所有槽位从本文件派生，
// 禁止在消费端裸算。
//
// 语义（frame = 绝对帧号，从 0 计）：
//   writeIndex    = frame % 3        —— 本帧唯一写入的槽
//   currentIndex  = frame % 3        —— DDGI 完成帧后本帧数据所在槽
//                                        （= writeIndex；显式命名以免
//                                        "读本帧槽"被误读为巧合复用）
//   previousIndex = (frame + 2) % 3  —— 上一完成帧（frame-1）的写槽，
//                                        本帧只读（BeginFrame fence 保证
//                                        frame-3 同槽写者已完成，
//                                        LumenDDGIPass.h 并发契约注释）
// ============================================================================
struct DDGIFrameResources {
    u32 writeIndex;
    u32 currentIndex;
    u32 previousIndex;
};

inline DDGIFrameResources DDGISlotsForFrame(u32 frame) {
    const u32 w = frame % 3u;
    return { w, w, (frame + 2u) % 3u };
}

} // namespace primal::graphics::lumen
