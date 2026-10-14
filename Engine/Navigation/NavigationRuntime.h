#pragma once

// NavigationRuntime.h — 导航运行态（ai-navigation S1 收口，2026-10-15）。
//
// 进程级全局：当前导航资产 + 按实体键控的移动任务表；tick 在 PipelineTick 的
// nav 段推进（帧序契约：script → nav → physics → pfx → bindings——脚本先产出
// 意图，导航随后推进并回写实体 transform，物理最后步进）。无资产的进程调
// tick 是空操作（编辑器无 navagent 时零成本）。
// 与 physics 的全局单例同型（SimulationAPI 门面 + cpp 持状态）。

#include "Navigation/NavigationService.h"

namespace primal::navigation::runtime {

// 全局资产：烘焙（内存）或加载烘焙文件；失败保持原状。
bool bake(const bake_input& in, const agent_profile& p);
bool load_asset_file(const char* path);
bool has_asset();
u32  generation();

// 任务面（宿主/Lua 共用）：move_to 以实体当前 transform 为起点建任务，
// 同实体重复调用 = 替换旧任务（重复请求语义）。
nav_error   move_to(u64 entity_id, const math::v3& target, f32 speed);   // speed<=0 用缺省 2m/s
void        stop(u64 entity_id);
move_state  state_of(u64 entity_id);      // 无任务 = idle
nav_error   failure_of(u64 entity_id);    // 无任务 = ok

// 每帧推进全部任务并回写实体 transform；实体失效的任务被剔除。
// arrived/failed_stale 各日志一次（事件语义由任务层保证恰一次）。
void tick(f32 dt);

void remove_entity(u64 entity_id);        // 实体销毁反注册
void clear();                             // 资产 + 任务全清（宿主 teardown）

} // namespace primal::navigation::runtime
