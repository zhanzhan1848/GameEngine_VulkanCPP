// GameplayAPI.cpp — 玩法规则骨架 S1 导出实现（薄转发到 gameplay::runtime，
// 与 NavigationAPI 同纪律：不做业务逻辑，POD↔引擎描述在此翻译）。

#include "GameplayAPI.h"
#include "Gameplay/GameplayRuntime.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace primal;

static gameplay::gp_predicate from_pod(const gameplay_predicate_pod& p)
{
    gameplay::gp_predicate d;
    d.kind = (gameplay::predicate_kind)p.kind;
    d.op   = (gameplay::compare_op)p.op;
    d.entity_a = p.entity_a;
    d.entity_b = p.entity_b;
    d.key  = p.key;
    d.value = p.value;
    return d;
}

extern "C" {

EDITOR_INTERFACE void GameplayClear(void)
{
    gameplay::runtime::clear();
}

EDITOR_INTERFACE const char* GameplayLastError(void)
{
    return gameplay::runtime::last_error();
}

EDITOR_INTERFACE unsigned int GameplayRegisterNamedEntity(unsigned long long entity,
                                                          const char* name)
{
    return (unsigned int)gameplay::runtime::register_named_entity((u64)entity, name);
}

EDITOR_INTERFACE unsigned int GameplayMachineBegin(unsigned long long entity,
                                                   const char* initial_state)
{
    return (unsigned int)gameplay::runtime::machine_begin((u64)entity, initial_state);
}

EDITOR_INTERFACE unsigned int GameplayMachineAddState(const char* name)
{
    return (unsigned int)gameplay::runtime::machine_add_state(name);
}

EDITOR_INTERFACE unsigned int GameplayMachineAddTransition(const char* from,
                                                           const char* to,
                                                           const char* event_or_null,
                                                           unsigned int manual)
{
    return (unsigned int)gameplay::runtime::machine_add_transition(from, to, event_or_null, manual != 0);
}

EDITOR_INTERFACE unsigned int GameplayMachineAddGuard(unsigned int transition_index,
                                                      const gameplay_predicate_pod* pred)
{
    if (!pred) return (unsigned int)gameplay::gp_error::invalid_argument;
    return (unsigned int)gameplay::runtime::machine_add_guard(
        transition_index, from_pod(*pred));
}

EDITOR_INTERFACE unsigned int GameplayMachineEnd(void)
{
    return (unsigned int)gameplay::runtime::machine_end();
}

EDITOR_INTERFACE unsigned int GameplayTriggerAdd(unsigned long long entity,
                                                 const gameplay_predicate_pod* pred,
                                                 const char* event_name,
                                                 unsigned int mode)
{
    if (!pred) return (unsigned int)gameplay::gp_error::invalid_argument;
    if (mode > 1) return (unsigned int)gameplay::gp_error::invalid_argument;
    return (unsigned int)gameplay::runtime::trigger_add(
        (u64)entity, from_pod(*pred), event_name, (gameplay::trigger_mode)mode);
}

EDITOR_INTERFACE unsigned int GameplayGetState(unsigned long long entity,
                                               char* out_state, unsigned int cap)
{
    if (!out_state || cap == 0) return (unsigned int)gameplay::gp_error::invalid_argument;
    std::string s;
    if (!gameplay::runtime::state_get((u64)entity, s))
        return (unsigned int)gameplay::gp_error::no_machine;
    std::snprintf(out_state, cap, "%s", s.c_str());
    return (unsigned int)gameplay::gp_error::ok;
}

EDITOR_INTERFACE unsigned int GameplayRequestTransition(unsigned long long entity,
                                                        const char* to,
                                                        char* out_detail, unsigned int cap)
{
    std::string detail;
    const gameplay::gp_error err =
        gameplay::runtime::state_transition((u64)entity, to, detail);
    if (out_detail && cap > 0)
        std::snprintf(out_detail, cap, "%s", detail.c_str());
    return (unsigned int)err;
}

EDITOR_INTERFACE unsigned int GameplayTraceRecordCount(void)
{
    return gameplay::runtime::trace_record_count();
}

EDITOR_INTERFACE unsigned int GameplayTraceGetRecord(unsigned int index,
                                                     gameplay_trace_record_pod* out)
{
    if (!out) return (unsigned int)gameplay::gp_error::invalid_argument;
    u32 frame = 0; gameplay::trace_kind kind{}; u64 entity = 0; std::string detail;
    if (!gameplay::runtime::trace_get(index, frame, kind, entity, detail))
        return (unsigned int)gameplay::gp_error::bad_index;
    out->frame = frame;
    out->kind = (unsigned int)kind;
    out->entity = entity;
    std::snprintf(out->detail, sizeof(out->detail), "%s", detail.c_str());
    return (unsigned int)gameplay::gp_error::ok;
}

EDITOR_INTERFACE unsigned int GameplayFrameCount(void)
{
    return gameplay::runtime::frame_count();
}

}  // extern "C"
