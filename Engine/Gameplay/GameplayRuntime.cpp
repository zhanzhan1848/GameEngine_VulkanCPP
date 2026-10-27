#include "Gameplay/GameplayRuntime.h"

// GameplayRuntime.cpp — L1 解释器实现。分层纪律：本文件只依赖
// ScriptEventBus（事件通道）/ ScriptState（属性读）/ GameEntity+Transform
//（距离求值），不碰渲染/物理/流体；谓词求值三态（false/true/unknown），
// unknown 一律按 false 并记一次 trace warn（"谓词越界有确定语义"）。

#include "Common/CoreLogStream.h"
#include "Components/Entity.h"
#include "Components/ScriptEventBus.h"
#include "Components/ScriptState.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace primal::gameplay::runtime {

namespace {

constexpr u32 kMaxStates        = 64;
constexpr u32 kMaxTransitions   = 256;
constexpr u32 kMaxGuardsPerEdge = 16;
constexpr u32 kMaxTriggers      = 256;
constexpr u32 kMaxMachines      = 256;
constexpr u32 kTraceCapacity    = 65536;

struct transition_def {
    std::string             from;
    std::string             to;
    std::string             event;   // 空 = 转移完成不发事件
    std::vector<gp_predicate> guards; // AND
    bool                    manual = false;  // true = 不参与自动求值，仅供 Lua 受控转移
};

struct machine_def {  // 构建期
    u64                       owner = 0;
    std::string               initial;
    std::vector<std::string>  states;
    std::vector<transition_def> transitions;
};

struct machine_instance {
    u64                       owner = 0;
    std::string               current;
    f32                       time_in_state = 0.f;
    std::vector<std::string>  states;               // 校验后的状态表（快照）
    std::vector<transition_def> transitions;
    std::unordered_map<std::string, u32> counts;    // 事件计数：当前状态进入以来
    std::unordered_set<std::string>     referenced; // 订阅的事件名集合
};

struct trigger_instance {
    u64                       owner = 0;
    gp_predicate              pred;
    std::string               event;
    trigger_mode              mode = trigger_mode::once;
    bool                      prev  = false;   // 上帧布尔投影（unknown→false）
    bool                      armed = true;    // once 触发后解除
    f32                       age   = 0.f;     // 创建以来秒数（timer 谓词用）
    std::unordered_map<std::string, u32> counts;   // 创建以来计数
    std::unordered_set<std::string>     referenced;
};

struct subscription_slot {
    u64  sub_id = 0;
    u32  refs   = 0;
};

struct runtime_state {
    std::vector<machine_instance> machines;              // 创建序（确定性遍历）
    std::unordered_map<u64, u32>  machine_by_entity;     // entity → machines 下标
    std::vector<trigger_instance> triggers;              // 声明序
    bool                          has_builder = false;
    machine_def                   builder;
    std::unordered_map<std::string, u64> name_to_entity;
    std::unordered_map<u64, std::string> entity_to_name;
    std::unordered_map<std::string, subscription_slot> name_subs;

    // trace
    struct trace_rec {
        u32        frame = 0;
        trace_kind kind  = trace_kind::trigger_fired;
        u64        entity = 0;
        std::string detail;
    };
    std::vector<trace_rec> trace;
    bool                   trace_overflow = false;
    u32                    frame = 0;
    std::string            error;             // last_error 缓冲
    std::unordered_set<std::string> warned;   // warn-once 去重
};

runtime_state& rt()
{
    static runtime_state s;
    return s;
}

void set_error(const std::string& msg)
{
    rt().error = msg;
}

void trace_push(trace_kind kind, u64 entity, const std::string& detail)
{
    auto& r = rt();
    if (r.trace.size() >= kTraceCapacity) {
        if (!r.trace_overflow) {
            r.trace_overflow = true;
            std::fprintf(stderr, "gameplay: trace capacity reached (%u) — further records dropped\n", kTraceCapacity);
        }
        return;
    }
    r.trace.push_back({ r.frame, kind, entity, detail });
}

void warn_once(u64 entity, const std::string& what)
{
    auto& r = rt();
    if (r.warned.insert(what).second) {
        std::fprintf(stderr, "gameplay: warn %s\n", what.c_str());
        trace_push(trace_kind::warn, entity, "what=" + what);
    }
}

// ---- 名字解析（惰性：求值时查表；未注册→unknown）----
bool resolve_entity(const std::string& ref, u64 owner, u64& out)
{
    if (ref.empty() || ref == "self") { out = owner; return true; }
    auto& r = rt();
    auto it = r.name_to_entity.find(ref);
    if (it == r.name_to_entity.end()) {
        warn_once(owner, "unknown_entity_name=" + ref);
        return false;
    }
    out = it->second;
    return true;
}

const char* owner_label(u64 entity)
{
    auto& r = rt();
    auto it = r.entity_to_name.find(entity);
    static thread_local char buf[32];
    if (it != r.entity_to_name.end()) return it->second.c_str();
    std::snprintf(buf, sizeof(buf), "id=%llu", (unsigned long long)entity);
    return buf;
}

// ---- 事件发射（payload = "entity=<名>"；C++ 裸字节 → Lua trampoline 转 string）----
void emit_string_event(const char* event_name, u64 entity)
{
    std::string payload = std::string("entity=") + owner_label(entity);
    const size_t len = payload.size() + 1;
    char* buf = new char[len];
    std::memcpy(buf, payload.c_str(), len);
    script::script_event_bus::instance().emit_by_name(
        event_name, buf, (u64)len, [](void* p) { delete[] static_cast<char*>(p); });
}

// 事件名订阅（引用计数；handler 捕获名字，drain 时给所有引用方计数）。
void subscribe_name(const std::string& name)
{
    auto& r = rt();
    auto& slot = r.name_subs[name];
    if (slot.refs == 0) {
        std::string captured = name;
        slot.sub_id = script::script_event_bus::instance().subscribe_by_name(
            name.c_str(), nullptr,
            [captured](void*, const void*, u64) {
                auto& rr = rt();
                for (auto& m : rr.machines)
                    if (m.referenced.count(captured)) ++m.counts[captured];
                for (auto& t : rr.triggers)
                    if (t.referenced.count(captured)) ++t.counts[captured];
            });
    }
    ++slot.refs;
}

void unsubscribe_all()
{
    auto& r = rt();
    auto& bus = script::script_event_bus::instance();
    for (auto& [name, slot] : r.name_subs)
        if (slot.sub_id) bus.unsubscribe(slot.sub_id);
    r.name_subs.clear();
}

// ---- 谓词求值 ----
bool compare(f32 lhs, f32 rhs, compare_op op)
{
    switch (op) {
        case compare_op::lt:  return lhs <  rhs;
        case compare_op::lte: return lhs <= rhs;
        case compare_op::gt:  return lhs >  rhs;
        case compare_op::gte: return lhs >= rhs;
        case compare_op::eq:  return lhs == rhs;
        case compare_op::neq: return lhs != rhs;
    }
    return false;
}

// ctx_machine/ctx_trigger 二选一非空（timer/count 的作用域归属）。
eval_status eval_predicate(const gp_predicate& p, u64 owner,
                           const machine_instance* ctx_machine,
                           const trigger_instance* ctx_trigger)
{
    switch (p.kind) {
        case predicate_kind::property: {
            u64 target = 0;
            if (!resolve_entity(p.entity_a, owner, target)) return eval_status::unknown;
            if (!script::state().is_initialized()) {
                warn_once(owner, "script_state_uninitialized");
                return eval_status::unknown;
            }
            const std::vector<std::string> path{ "entities", std::to_string(target), p.key };
            script::StateValue v = script::state().get(path);
            f32 lhs = 0.f;
            if (v.tag == script::StateValue::Tag::Number)      lhs = (f32)v.n;
            else if (v.tag == script::StateValue::Tag::Bool)   lhs = v.b ? 1.f : 0.f;
            else {
                warn_once(owner, "property_missing=entities." + std::to_string(target) + "." + p.key);
                return eval_status::unknown;
            }
            return compare(lhs, p.value, p.op) ? eval_status::true_v : eval_status::false_v;
        }
        case predicate_kind::distance: {
            u64 a = 0, b = 0;
            if (!resolve_entity(p.entity_a, owner, a) ||
                !resolve_entity(p.entity_b, owner, b))
                return eval_status::unknown;
            const game_entity::entity ea{ game_entity::entity_id{ (u32)a } };
            const game_entity::entity eb{ game_entity::entity_id{ (u32)b } };
            if (!game_entity::is_alive(ea.get_id()) || !game_entity::is_alive(eb.get_id())) {
                warn_once(owner, "distance_entity_not_alive");
                return eval_status::unknown;
            }
            const math::v3 pa = ea.Get<component::Transform>().position();
            const math::v3 pb = eb.Get<component::Transform>().position();
            const f32 dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
            const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
            return compare(d, p.value, p.op) ? eval_status::true_v : eval_status::false_v;
        }
        case predicate_kind::timer: {
            const f32 t = ctx_machine ? ctx_machine->time_in_state
                                      : (ctx_trigger ? ctx_trigger->age : 0.f);
            return compare(t, p.value, p.op) ? eval_status::true_v : eval_status::false_v;
        }
        case predicate_kind::event_count: {
            const auto* counts = ctx_machine ? &ctx_machine->counts
                                             : (ctx_trigger ? &ctx_trigger->counts : nullptr);
            if (!counts) return eval_status::unknown;
            auto it = counts->find(p.key);
            const f32 n = (f32)(it != counts->end() ? it->second : 0u);
            return compare(n, p.value, p.op) ? eval_status::true_v : eval_status::false_v;
        }
    }
    return eval_status::unknown;
}

bool eval_bool(const gp_predicate& p, u64 owner,
               const machine_instance* m, const trigger_instance* t)
{
    return eval_predicate(p, owner, m, t) == eval_status::true_v;  // unknown → false
}

// 收集谓词里引用的事件名（count 谓词的 key）。
void collect_referenced(const gp_predicate& p, std::unordered_set<std::string>& out)
{
    if (p.kind == predicate_kind::event_count && !p.key.empty()) out.insert(p.key);
}

// ---- 转移执行（auto 与 lua 共用；trace cause 区分）----
void do_transition(machine_instance& m, const transition_def& t, bool from_lua)
{
    const std::string from = m.current;
    m.current = t.to;
    m.time_in_state = 0.f;
    m.counts.clear();
    trace_push(from_lua ? trace_kind::state_lua : trace_kind::state_auto, m.owner,
               "entity=" + std::string(owner_label(m.owner)) +
               "|from=" + from + "|to=" + t.to);
    if (!t.event.empty()) emit_string_event(t.event.c_str(), m.owner);
}

bool has_state(const std::vector<std::string>& states, const std::string& s)
{
    for (auto& x : states) if (x == s) return true;
    return false;
}

}  // namespace

// ==== 公共 API ====

void clear()
{
    unsubscribe_all();
    auto& r = rt();
    r.machines.clear();
    r.machine_by_entity.clear();
    r.triggers.clear();
    r.has_builder = false;
    r.builder = {};
    r.name_to_entity.clear();
    r.entity_to_name.clear();
    r.trace.clear();
    r.trace_overflow = false;
    r.frame = 0;
    r.warned.clear();
}

const char* last_error()
{
    return rt().error.c_str();
}

gp_error register_named_entity(u64 entity, const char* name)
{
    if (!name || !name[0]) { set_error("register_named_entity: empty name"); return gp_error::invalid_argument; }
    auto& r = rt();
    r.name_to_entity[name] = entity;
    r.entity_to_name[entity] = name;
    return gp_error::ok;
}

gp_error machine_begin(u64 entity, const char* initial_state)
{
    auto& r = rt();
    if (r.has_builder) { set_error("machine_begin: builder already open"); return gp_error::busy; }
    if (!initial_state || !initial_state[0]) { set_error("machine_begin: empty initial state"); return gp_error::invalid_argument; }
    if (r.machine_by_entity.count(entity)) { set_error("machine_begin: entity already has a machine"); return gp_error::already_exists; }
    if (r.machines.size() >= kMaxMachines)  { set_error("machine_begin: machine capacity"); return gp_error::overflow; }
    r.has_builder = true;
    r.builder = {};
    r.builder.owner = entity;
    r.builder.initial = initial_state;
    return gp_error::ok;
}

gp_error machine_add_state(const char* name)
{
    auto& r = rt();
    if (!r.has_builder) { set_error("machine_add_state: no builder open"); return gp_error::not_building; }
    if (!name || !name[0]) { set_error("machine_add_state: empty name"); return gp_error::invalid_argument; }
    if (has_state(r.builder.states, name)) { set_error(std::string("machine_add_state: duplicate state ") + name); return gp_error::already_exists; }
    if (r.builder.states.size() >= kMaxStates) { set_error("machine_add_state: state capacity"); return gp_error::overflow; }
    r.builder.states.push_back(name);
    return gp_error::ok;
}

gp_error machine_add_transition(const char* from, const char* to, const char* event_or_null,
                                 bool manual)
{
    auto& r = rt();
    if (!r.has_builder) { set_error("machine_add_transition: no builder open"); return gp_error::not_building; }
    if (!from || !from[0] || !to || !to[0]) { set_error("machine_add_transition: empty endpoint"); return gp_error::invalid_argument; }
    if (!has_state(r.builder.states, from)) { set_error(std::string("machine_add_transition: dangling from=") + from); return gp_error::dangling_state; }
    if (!has_state(r.builder.states, to))   { set_error(std::string("machine_add_transition: dangling to=") + to);   return gp_error::dangling_state; }
    if (r.builder.transitions.size() >= kMaxTransitions) { set_error("machine_add_transition: transition capacity"); return gp_error::overflow; }
    transition_def t;
    t.from = from;
    t.to = to;
    if (event_or_null && event_or_null[0]) t.event = event_or_null;
    t.manual = manual;
    r.builder.transitions.push_back(std::move(t));
    return gp_error::ok;
}

gp_error machine_add_guard(u32 transition_index, const gp_predicate& pred)
{
    auto& r = rt();
    if (!r.has_builder) { set_error("machine_add_guard: no builder open"); return gp_error::not_building; }
    if (transition_index >= r.builder.transitions.size()) {
        set_error("machine_add_guard: transition index out of range");
        return gp_error::bad_index;
    }
    auto& t = r.builder.transitions[transition_index];
    if (t.guards.size() >= kMaxGuardsPerEdge) {
        set_error("machine_add_guard: guard capacity per transition");
        return gp_error::overflow;
    }
    t.guards.push_back(pred);
    return gp_error::ok;
}

gp_error machine_end()
{
    auto& r = rt();
    if (!r.has_builder) { set_error("machine_end: no builder open"); return gp_error::not_building; }
    if (!has_state(r.builder.states, r.builder.initial)) {
        set_error("machine_end: initial state '" + r.builder.initial + "' not declared");
        trace_push(trace_kind::build_error, r.builder.owner, "stage=machine_end|message=initial_not_declared");
        r.has_builder = false; r.builder = {};
        return gp_error::dangling_state;
    }
    machine_instance m;
    m.owner = r.builder.owner;
    m.current = r.builder.initial;
    m.states = std::move(r.builder.states);
    m.transitions = std::move(r.builder.transitions);
    for (auto& t : m.transitions)
        for (auto& g : t.guards) collect_referenced(g, m.referenced);
    r.has_builder = false;
    r.builder = {};
    const u32 idx = (u32)r.machines.size();
    r.machines.push_back(std::move(m));
    r.machine_by_entity[r.machines[idx].owner] = idx;
    for (auto& name : r.machines[idx].referenced) subscribe_name(name);
    return gp_error::ok;
}

gp_error trigger_add(u64 entity, const gp_predicate& pred,
                     const char* event_name, trigger_mode mode)
{
    auto& r = rt();
    if (!event_name || !event_name[0]) { set_error("trigger_add: empty event name"); return gp_error::invalid_argument; }
    if (r.triggers.size() >= kMaxTriggers) { set_error("trigger_add: trigger capacity"); return gp_error::overflow; }
    trigger_instance t;
    t.owner = entity;
    t.pred = pred;
    t.event = event_name;
    t.mode = mode;
    collect_referenced(pred, t.referenced);
    r.triggers.push_back(std::move(t));
    for (auto& name : r.triggers.back().referenced) subscribe_name(name);
    return gp_error::ok;
}

void tick(f32 dt)
{
    auto& r = rt();

    // 机器：自动转移（每机器每 tick 至多一次；出边按声明序）。
    for (auto& m : r.machines) {
        m.time_in_state += dt;
        for (const auto& t : m.transitions) {
            if (t.from != m.current || t.manual) continue;
            bool pass = true;
            for (const auto& g : t.guards) {
                if (!eval_bool(g, m.owner, &m, nullptr)) { pass = false; break; }
            }
            if (pass) { do_transition(m, t, false); break; }
        }
    }

    // 触发器：上升沿（布尔投影；unknown→false）。
    for (auto& t : r.triggers) {
        t.age += dt;
        const bool v = eval_bool(t.pred, t.owner, nullptr, &t);
        if (v && !t.prev && t.armed) {
            emit_string_event(t.event.c_str(), t.owner);
            trace_push(trace_kind::trigger_fired, t.owner,
                       "event=" + t.event + "|entity=" + owner_label(t.owner));
            if (t.mode == trigger_mode::once) t.armed = false;
        }
        t.prev = v;
    }

    ++r.frame;
}

u32 frame_count() { return rt().frame; }

bool state_get(u64 entity, std::string& out)
{
    auto& r = rt();
    auto it = r.machine_by_entity.find(entity);
    if (it == r.machine_by_entity.end()) return false;
    out = r.machines[it->second].current;
    return true;
}

gp_error state_transition(u64 entity, const char* to, std::string& out_detail)
{
    auto& r = rt();
    if (!to || !to[0]) { set_error("state_transition: empty target"); return gp_error::invalid_argument; }
    auto it = r.machine_by_entity.find(entity);
    if (it == r.machine_by_entity.end()) {
        out_detail = "no_machine";
        return gp_error::no_machine;
    }
    auto& m = r.machines[it->second];
    for (const auto& t : m.transitions) {
        if (t.from == m.current && t.to == to) {
            do_transition(m, t, true);
            out_detail.clear();
            return gp_error::ok;
        }
    }
    out_detail = "no_transition:" + m.current + "->" + to;
    return gp_error::no_transition;
}

u32 trace_record_count() { return (u32)rt().trace.size(); }

bool trace_get(u32 index, u32& out_frame, trace_kind& out_kind,
               u64& out_entity, std::string& out_detail)
{
    auto& r = rt();
    if (index >= r.trace.size()) return false;
    const auto& rec = r.trace[index];
    out_frame = rec.frame;
    out_kind = rec.kind;
    out_entity = rec.entity;
    out_detail = rec.detail;
    return true;
}

}  // namespace primal::gameplay::runtime
