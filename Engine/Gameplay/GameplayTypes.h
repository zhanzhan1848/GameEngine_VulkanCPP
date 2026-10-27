#pragma once

// GameplayTypes.h — 玩法规则骨架 L0 数据契约（gameplay-rules.md S1）。
//
// 封闭谓词集：property / distance / timer / event_count 四类，task（S2）
// 复用同一集合；新谓词类型只能经注册加入本文件，不为样例扩集。
//
// 设计约束（planning 冻结项）：
//   - 契约是纯数据（无函数指针/无代码 guard），业务身份用名字引用，
//     不以原生地址/数组位置作持久引用；
//   - 同输入同帧序同事件流：求值顺序（机器声明序→转移声明序→触发器
//     声明序）与 dt 累计（f32）共同决定可回放性；
//   - 解释器 tick 位置冻结在 PipelineTick 的 script frame_tick 之后、
//     物理步进之前（见 EngineDLL/PhysicsAPI.cpp 帧序契约注释）。

#include "Common/PrimitiveTypes.h"

#include <string>

namespace primal::gameplay {

// ---- 封闭谓词集（S1 冻结四类；扩展=修改本枚举+求值器，不开旁路）----
enum class predicate_kind : u8 {
    property    = 0,  // state.entities.<id>.<key> 与 value 比较（Number/Bool）
    distance    = 1,  // 两实体 transform 距离与 value 比较（米）
    timer       = 2,  // 机器：当前状态停留时间；独立触发器：创建以来时间（秒）
    event_count = 3,  // 事件名计数：机器按当前状态进入以来；独立触发器按创建以来
};

enum class compare_op : u8 {
    lt = 0, lte, gt, gte, eq, neq,
};

enum class trigger_mode : u8 {
    once   = 0,  // 首次上升沿触发后永久解除
    refire = 1,  // 谓词回落后重新武装（再上升沿再触发）
};

// 任务结局（S2）：active 从创建起算；completed/failed 终态（事件/条件/Lua
// 三路来源，先到先得，trace 记 cause）。
enum class task_state : u8 {
    active    = 0,
    completed = 1,
    failed    = 2,
};

// 求值三态：谓词越界/名字未注册/属性缺失 → unknown（按 false 处理），
// 并在 trace 里记一条 warn——"谓词越界有确定语义"的落点。
enum class eval_status : u8 { false_v = 0, true_v, unknown };

// 引擎内部谓词描述（字符串持有；C ABI 镜像见 EngineDLL/GameplayAPI.h）。
struct gp_predicate {
    predicate_kind kind = predicate_kind::property;
    compare_op     op = compare_op::lt;
    std::string    entity_a;   // ""（或 "self"）= 所属实体自身
    std::string    entity_b;   // 仅 distance 用
    std::string    key;        // property 键 / event_count 事件名
    f32            value = 0.f;
};

// 错误码（C ABI 直接透传为 u32；0 恒为 ok）。
enum class gp_error : u32 {
    ok              = 0,
    invalid_argument= 1,   // 空名/null 指针/非法枚举
    not_building    = 2,   // Add* 调用时没有打开的机器构建
    already_exists  = 3,   // 重复状态名 / 实体已有机器
    dangling_state  = 4,   // 转移引用了未声明状态 / 初始态不在状态表
    bad_index       = 5,   // guard 引用的转移序号越界
    no_machine      = 6,   // 查询/转移请求时实体没有机器
    no_transition   = 7,   // Lua 请求的 (当前态→目标态) 边不存在
    overflow        = 8,   // 内部上限（状态数/谓词数/trace 容量）
    busy            = 9,   // 已有机器在构建中（machine_begin 重入）
};

// trace 记录种类（事件/帧号/实体可关联的最小闭环；S2 dry-run 复用）。
enum class trace_kind : u32 {
    trigger_fired  = 0,  // detail: event=<name>|entity=<owner>|predicate=<...>
    state_auto     = 1,  // detail: entity=<owner>|from=..|to=..
    state_lua      = 2,  // 同上，cause=lua
    build_error    = 3,  // detail: stage=..|message=..
    task_completed = 4,  // S2
    task_failed    = 5,  // S2
    warn           = 6,  // detail: what=..（未知名字/属性缺失，一次性）
};

inline const char* to_string(gp_error e)
{
    switch (e) {
        case gp_error::ok:               return "ok";
        case gp_error::invalid_argument: return "invalid_argument";
        case gp_error::not_building:     return "not_building";
        case gp_error::already_exists:   return "already_exists";
        case gp_error::dangling_state:   return "dangling_state";
        case gp_error::bad_index:        return "bad_index";
        case gp_error::no_machine:       return "no_machine";
        case gp_error::no_transition:    return "no_transition";
        case gp_error::overflow:         return "overflow";
        case gp_error::busy:             return "busy";
    }
    return "unknown";
}

}  // namespace primal::gameplay
