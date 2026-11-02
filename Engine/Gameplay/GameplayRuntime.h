#pragma once

// GameplayRuntime.h — 玩法规则骨架 L1 确定性解释器（进程级运行态）。
// 与 NavigationRuntime 同构：namespace 自由函数 + 隐藏静态态，主线程契约
//（事件总线 drain / PipelineTick / Lua hook 全在主线程）。
//
// 求值顺序（确定性契约，勿改）：
//   每个 gameplay::tick(dt) 先按机器创建序推进各机器（每机器每 tick 至多
//   一次自动转移：当前状态的出边按声明序取第一条全 guard 通过者），再按
//   触发器声明序做上升沿检测。Lua 经 game.state.transition 的受控写发生在
//   script frame_tick 的 bus drain 内（同帧更早），立即生效。
//
// 事件流向：解释器经 ScriptEventBus 字符串通道（emit_by_name）发事件，
// payload 为 "entity=<名>|k=v" 形式的 NUL 结尾字节串；本帧发射下帧 drain
// 被 Lua bus.on 处理（drain 在 frame_tick 内、解释器 tick 之前——加一帧
// 确定性延迟，属契约而非缺陷）。

#include "Gameplay/GameplayTypes.h"

#include <string>

namespace primal::gameplay::runtime {

// ---- 生命周期 ----
// 清空全部实例/构建器/名字表/订阅/trace（场景卸载或测试隔离时调）。
void clear();
// 最近一次错误的可读描述（静态缓冲，下一次调用失效；主线程）。
const char* last_error();

// ---- 名字表（实体名 ↔ entity_id，宿主在实体创建后登记）----
gp_error register_named_entity(u64 entity, const char* name);

// ---- 状态机构建器（Begin..End 原子提交；End 校验失败整组丢弃）----
gp_error machine_begin(u64 entity, const char* initial_state);
gp_error machine_add_state(const char* name);
// event_or_null：转移完成时发射的事件名；null/空 = 不发事件。manual=true 的
// 转移不参与自动求值（决策权留给 Lua game.state.transition），但仍受结构校验。
gp_error machine_add_transition(const char* from, const char* to, const char* event_or_null,
                                bool manual = false);
// guard 追加到 transition_index（0 起，按 add 顺序）的转移上，多条为 AND。
gp_error machine_add_guard(u32 transition_index, const gp_predicate& pred);
gp_error machine_end();

// ---- 独立触发器（不依赖机器存在；挂实体的声明序边沿触发）----
gp_error trigger_add(u64 entity, const gp_predicate& pred,
                     const char* event_name, trigger_mode mode);

// ---- 任务契约（S2）：目标+完成/失败条件，复用同一封闭谓词集 ----
// 条件求值作用域：timer/count 按任务创建以来计（与机器的状态作用域区分）。
// 终态先到先得：每 tick 先查完成集（全真）再查失败集（全真）。
gp_error task_begin(u64 entity, const char* task_id);
// kind：0 = 完成条件，1 = 失败条件（同集多条为 AND）。
gp_error task_add_condition(u8 kind, const gp_predicate& pred);
gp_error task_end();
// 查询：实体无此任务返回 false。
bool task_get(u64 entity, const char* task_id, task_state& out);
// Lua 受控终局（game.task.complete/fail）：非终态时生效并记 trace cause=lua。
gp_error task_force(u64 entity, const char* task_id, bool complete, std::string& out_detail);

// ---- 每帧推进（PipelineTick 的 script 段之后调用；无实例时零成本）----
void tick(f32 dt);
u32  frame_count();  // 已完成的 gameplay tick 数

// ---- 查询与 Lua 受控写入口 ----
// 实体当前状态名；无机器返回 false。
bool state_get(u64 entity, std::string& out);
// Lua 侧受控转移：要求 (当前态→to) 边存在于转移表（结构校验；guard 只
// 约束自动路径——Lua 是决策者）。成功 0；失败返回错误码并填 out_detail。
gp_error state_transition(u64 entity, const char* to, std::string& out_detail);

// ---- trace（事件/帧号/实体可关联；S1 回放确定性门禁 + S2 dry-run 复用）----
u32  trace_record_count();
// 越界返回 false。
bool trace_get(u32 index, u32& out_frame, trace_kind& out_kind,
               u64& out_entity, std::string& out_detail);

}  // namespace primal::gameplay::runtime
