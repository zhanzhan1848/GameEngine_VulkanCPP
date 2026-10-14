#pragma once

// NavigationService.h — 导航资产外壳与移动任务生命周期（ai-navigation S1）。
//
// S1 边界（阶段合同）：本层不含编辑器 UI/组件；Player 接线在后续切片。
// 确定性纪律：task_update 是纯推进函数（同输入同输出），事件恰好触发一次。

#include "Navigation/NavGrid.h"

namespace primal::navigation {

// ---- 烘焙资产（编辑器保存/加载的最小外壳）----
struct navigation_asset {
    nav_grid grid;
    u32      generation{ 0 };   // 每次 bake/加载单调 +1；任务快照用它检测过期（stale_nav）
};

// 烘焙并替换资产内容（失败时资产保持原状）。
nav_error asset_bake(navigation_asset& a, const bake_input& in, const agent_profile& p);

// 确定性文本落盘/读回；坏文件给类型化错误，不静默。
nav_error asset_save(const navigation_asset& a, const char* path);
nav_error asset_load(navigation_asset& a, const char* path);

// ---- 移动任务 ----
struct move_task {
    u64  id{ 0 };                        // 进程内唯一（从 1 起）
    math::v3 target{};
    move_state state{ move_state::idle };
    nav_error  failure{ nav_error::ok }; // state==failed 时的失败码
    u32  generation{ 0 };                // 创建时的资产代际快照
    std::vector<math::v3> path;          // 含起点与目标投影
    u32  next_waypoint{ 1 };             // 下一个待到达航点（0 是起点自身）
    bool arrived_event_fired{ false };
    f32  arrival_radius{ 0.1f };         // 合同 A01：距目标 ≤0.1m 完成
    f32  speed{ 2.f };                   // 米/秒
};

// 创建任务：路径规划失败时 out.state=failed、out.failure=对应码（不抛异常）。
nav_error task_create(const navigation_asset& a, const math::v3& from, const math::v3& to,
                      f32 speed, f32 nearest_radius, move_task& out);

// 推进一步：沿路径以 speed*dt 前进，返回 out_pos 与本次事件。
// 合同：取消后不再向旧目标移动（直接返回 current）；arrived 恰好一次。
move_event task_update(const navigation_asset& a, move_task& t, const math::v3& current,
                       f32 dt, math::v3& out_pos);

// 取消任务（幂等）。
void task_cancel(move_task& t);

} // namespace primal::navigation
