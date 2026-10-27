#pragma once

// GameplayAPI.h — 玩法规则骨架（gameplay-rules.md S1）的 EngineDLL 导出面。
// 只增不改（ABI 纪律）：新能力=新导出；已发布签名/POD 布局不得变动。
// 契约语义与求值顺序冻结项见 Engine/Gameplay/GameplayTypes.h / GameplayRuntime.h。

#include "Common.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- POD 镜像：封闭谓词集（C ABI 传输形态；引擎内描述见 GameplayTypes.h）----
// kind/op 枚举值与 Engine/Gameplay/GameplayTypes.h 逐位对应：
//   kind: 0 property, 1 distance, 2 timer, 3 event_count
//   op:   0 lt, 1 lte, 2 gt, 3 gte, 4 eq, 5 neq
//   entity_a == "" 或 "self" 表示所属实体自身；entity_b 仅 distance 用；
//   key 为 property 的属性键 / event_count 的事件名；value 为比较右值
//   （timer 秒 / count 次 / 距离米 / 属性值）。
typedef struct gameplay_predicate_pod {
    unsigned char  kind;
    unsigned char  op;
    unsigned short reserved;
    char           entity_a[32];
    char           entity_b[32];
    char           key[64];
    float          value;
    float          spare;
} gameplay_predicate_pod;

static_assert(sizeof(gameplay_predicate_pod) == 140,
              "gameplay_predicate_pod ABI must match PrimalEditor_Avalonia mirror");

// ---- POD 镜像：trace 记录（事件/帧号/实体可关联的最小闭环）----
typedef struct gameplay_trace_record_pod {
    unsigned int frame;        // 记录时刻已完成的 gameplay tick 数
    unsigned int kind;         // gameplay trace_kind 枚举值
    unsigned long long entity; // 所属实体（完整 entity_id）
    char          detail[104]; // NUL 结尾 "k=v|k=v" 形式
} gameplay_trace_record_pod;

static_assert(sizeof(gameplay_trace_record_pod) == 120,
              "gameplay_trace_record_pod ABI must match PrimalEditor_Avalonia mirror");

// 错误码（u32 返回；0 恒为 ok）——与 Engine/Gameplay/GameplayTypes.h gp_error
// 逐位对应：1 invalid_argument, 2 not_building, 3 already_exists,
// 4 dangling_state, 5 bad_index, 6 no_machine, 7 no_transition, 8 overflow, 9 busy。

// ---- 生命周期 ----
EDITOR_INTERFACE void         GameplayClear(void);
EDITOR_INTERFACE const char*  GameplayLastError(void);

// ---- 名字表（宿主在实体创建后登记；谓词按名惰性解析）----
EDITOR_INTERFACE unsigned int GameplayRegisterNamedEntity(unsigned long long entity,
                                                          const char* name);

// ---- 状态机构建器（Begin..End 原子提交；End 校验失败整组丢弃）----
EDITOR_INTERFACE unsigned int GameplayMachineBegin(unsigned long long entity,
                                                   const char* initial_state);
EDITOR_INTERFACE unsigned int GameplayMachineAddState(const char* name);
// manual != 0：转移不参与自动求值（Lua 决策边），结构校验仍然生效。
EDITOR_INTERFACE unsigned int GameplayMachineAddTransition(const char* from,
                                                           const char* to,
                                                           const char* event_or_null,
                                                           unsigned int manual);
EDITOR_INTERFACE unsigned int GameplayMachineAddGuard(unsigned int transition_index,
                                                      const gameplay_predicate_pod* pred);
EDITOR_INTERFACE unsigned int GameplayMachineEnd(void);

// ---- 任务（S2）----
// 条件集复用封闭谓词；终态先到先得（cond/Lua 两路）。state：0 active、
// 1 completed、2 failed（gameplay::task_state 逐位对应）。
EDITOR_INTERFACE unsigned int GameplayTaskBegin(unsigned long long entity, const char* task_id);
EDITOR_INTERFACE unsigned int GameplayTaskAddCondition(unsigned int kind /*0 complete 1 fail*/,
                                                       const gameplay_predicate_pod* pred);
EDITOR_INTERFACE unsigned int GameplayTaskEnd(void);
// 返回错误码；成功时 out_state 填 task_state 数值。找不到返回 6 (no_machine)。
EDITOR_INTERFACE unsigned int GameplayTaskGetState(unsigned long long entity, const char* task_id,
                                                   unsigned int* out_state);

// ---- 独立触发器 ----
// mode: 0 once（首次上升沿后永久解除），1 refire（回落后重新武装）。
EDITOR_INTERFACE unsigned int GameplayTriggerAdd(unsigned long long entity,
                                                 const gameplay_predicate_pod* pred,
                                                 const char* event_name,
                                                 unsigned int mode);

// ---- 查询（宿主/测试面；脚本走 Lua game 表同一运行态）----
// 返回错误码；成功时 out_state 填当前状态名（NUL 结尾）。
EDITOR_INTERFACE unsigned int GameplayGetState(unsigned long long entity,
                                               char* out_state, unsigned int cap);
// 受控转移（同 game.state.transition 语义）。
EDITOR_INTERFACE unsigned int GameplayRequestTransition(unsigned long long entity,
                                                        const char* to,
                                                        char* out_detail, unsigned int cap);

// ---- trace ----
EDITOR_INTERFACE unsigned int GameplayTraceRecordCount(void);
EDITOR_INTERFACE unsigned int GameplayTraceGetRecord(unsigned int index,
                                                     gameplay_trace_record_pod* out);
EDITOR_INTERFACE unsigned int GameplayFrameCount(void);

#ifdef __cplusplus
}
#endif
