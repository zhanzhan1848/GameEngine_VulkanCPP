// Phase 5: Engine-side debug draw queue.
//
// EngineDLL 的 DebugDrawAPI 是 C ABI 入口，但实际队列必须住在 Engine 内部 ——
// ForwardSceneRenderer 的 Pass 6 才能拿到。本头文件定义一个线程安全的
// 单例队列，EngineDLL 写入，ForwardSceneRenderer 每帧 drain。
//
// 颜色当前被丢弃（LineBatchRenderer 只接受 position）；保留 rgb 字段以便
// 后续给 LineBatchRenderer 加 per-vertex color 时不破坏 API。

#pragma once

#include "CommonHeaders.h"
#include "Utilities/MathTypes.h"
#include <vector>

namespace primal::graphics::debug_draw {

struct DebugLine {
    math::v3 a;
    math::v3 b;
    u32     rgb;
};

// 全局单例队列。Meyers singleton —— C++11 起函数局部 static 初始化线程安全。
std::vector<DebugLine>& queue();

// 写入端 API（EngineDLL 调用）
void add_line(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, u32 rgb);
void clear();

// 读取端 API（ForwardSceneRenderer 调用）
// 把队列内容拷贝到 out，然后清空内部队列。返回拷贝的 line 数。
u32 drain_into(std::vector<DebugLine>& out);

u32 line_count();

} // namespace primal::graphics::debug_draw
