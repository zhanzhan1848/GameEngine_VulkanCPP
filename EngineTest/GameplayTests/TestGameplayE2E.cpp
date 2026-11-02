// TestGameplayE2E.cpp — gameplay S1 端到端：靠近按钮 → 触发器发事件 →
// Lua bus.on 收到 → game.state.transition（受控写）→ door.opened → Lua 挪门
//（entity.set_position）→ 远离后 guard 自动关门。全链走 EngineDLL 导出
//（ScriptEnsureSystems/ScriptCreateForEntity/PipelineTick/Gameplay*）——
// 与 PrimalPlayer 同一路径，无测试后门。
// 验收项：保存重开（清空+重建）与首轮行为一致（同输入同帧序同事件流）。

// 纯 C ABI 测试：本可执行文件只消费 EngineDLL 导出，绝不直接引引擎 C++
// 符号——否则静态 Engine 与 libEngineDLL.dylib 双副本单例（transform 向量/
// gameplay 运行态各一份）读到空副本（实测 Vector.h 断言）。

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ---- EngineDLL C ABI 声明镜像（与 PrimalPlayer 同纪律）----
extern "C" {
std::uint32_t ScriptEnsureSystems(void);
std::uint64_t ScriptCreateForEntity(std::uint32_t entity_id, const char* lua_path,
                                    const char** param_keys, const char** param_values,
                                    std::uint32_t param_count);
void ScriptDestroyForEntity(std::uint64_t script_id);
std::uint32_t CreateEntity(float pos_x, float pos_y, float pos_z);
void SetEntityTransform(std::uint32_t id, const float* position,
                        const float* rotation, const float* scale);
void DestroyEntity(std::uint32_t entity_id);
void PipelineTick(float dt_seconds);
void GameplayClear(void);
const char* GameplayLastError(void);
std::uint32_t GameplayRegisterNamedEntity(unsigned long long entity, const char* name);
std::uint32_t GameplayMachineBegin(unsigned long long entity, const char* initial_state);
std::uint32_t GameplayMachineAddState(const char* name);
std::uint32_t GameplayMachineAddTransition(const char* from, const char* to,
                                           const char* event_or_null, std::uint32_t manual);
std::uint32_t GameplayMachineAddGuard(unsigned int transition_index,
                                      const void* predicate_pod);
std::uint32_t GameplayMachineEnd(void);
std::uint32_t GameplayTriggerAdd(unsigned long long entity, const void* predicate_pod,
                                 const char* event_name, unsigned int mode);
std::uint32_t GameplayGetState(unsigned long long entity, char* out_state, std::uint32_t cap);
std::uint32_t GameplayTraceRecordCount(void);
std::uint32_t GameplayTraceGetRecord(unsigned int index, void* out_record_pod);
std::uint32_t GameplayTaskBegin(unsigned long long entity, const char* task_id);
std::uint32_t GameplayTaskAddCondition(std::uint32_t kind, const void* predicate_pod);
std::uint32_t GameplayTaskEnd(void);
std::uint32_t GameplayTaskGetState(unsigned long long entity, const char* task_id,
                                   std::uint32_t* out_state);
std::uint32_t GetEntityPosition(std::uint32_t entity_id, float* out_pos3);
}

// 引擎侧 transform 读取（门位移断言）：直接用 Engine 头（本测试链 EngineDLL
// 的静态 Engine，符号可见）。

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestGameplayE2E] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestGameplayE2E] ok: " << name << std::endl;
    }
}

struct PredicatePod {
    std::uint8_t kind, op;
    std::uint16_t reserved;
    char entity_a[32];
    char entity_b[32];
    char key[64];
    float value;
    float spare;
};
static_assert(sizeof(PredicatePod) == 140, "PredicatePod mirrors gameplay_predicate_pod");

constexpr float kDoorOpenY = 3.f;

// trace POD 镜像（120B，布局见 EngineDLL/GameplayAPI.h）
struct TraceRecordPod {
    std::uint32_t frame, kind;
    unsigned long long entity;
    char detail[104];
};
static_assert(sizeof(TraceRecordPod) == 120, "TraceRecordPod mirrors gameplay_trace_record_pod");

// 每轮：建三实体 + 门脚本 + 门机器 + 按钮触发器，跑 120 帧（靠近→远离），
// 返回 (trace 摘要, 门最终位移, 各阶段状态快照)。
struct RunResult {
    std::vector<std::string> trace;
    float door_y = 0.f;
    bool opened  = false;
    bool closed  = false;
    bool task_completed = false;
};

RunResult run_round(const std::string& lua_path)
{
    GameplayClear();

    const std::uint32_t player = CreateEntity(10.f, 0.f, 0.f);
    const std::uint32_t button = CreateEntity(0.f, 0.f, 0.f);
    const std::uint32_t door   = CreateEntity(0.f, 0.f, -2.f);
    RunResult res;

    GameplayRegisterNamedEntity(player, "Player");
    GameplayRegisterNamedEntity(button, "Button");
    GameplayRegisterNamedEntity(door, "Door");

    // 门脚本：收 button.near → 受控转移 open；收 door.opened → 抬门 + 记属性。
    const std::uint64_t script = ScriptCreateForEntity(door, lua_path.c_str(), nullptr, nullptr, 0);
    check(script != 0xFFFFFFFFFFFFFFFFull, "door script attached");

    // 门机器：closed --(Lua 请求)--> open --(远离 guard)--> closed
    check(GameplayMachineBegin(door, "closed") == 0, "door machine begin");
    GameplayMachineAddState("closed");
    GameplayMachineAddState("open");
    check(GameplayMachineAddTransition("closed", "open", "door.opened", 1u) == 0, "trans open (manual)");
    check(GameplayMachineAddTransition("open", "closed", "door.closed", 0u) == 0, "trans close (auto)");
    PredicatePod away;
    away.kind = 1; away.op = 3;   // dist, gte
    std::snprintf(away.entity_a, sizeof(away.entity_a), "Player");
    std::snprintf(away.entity_b, sizeof(away.entity_b), "Button");
    away.value = 5.f;
    check(GameplayMachineAddGuard(1, &away) == 0, "guard dist Player-Button gte 5");
    check(GameplayMachineEnd() == 0, "door machine end");

    // S2：门任务——完成=脚本属性 opened gte 1（door.opened 处理器写入）。
    check(GameplayTaskBegin(door, "open_door") == 0, "task begin open_door");
    PredicatePod opened_pred;
    opened_pred.kind = 0; opened_pred.op = 3;   // prop, gte
    std::snprintf(opened_pred.entity_a, sizeof(opened_pred.entity_a), "self");
    std::snprintf(opened_pred.key, sizeof(opened_pred.key), "opened");
    opened_pred.value = 1.f;
    check(GameplayTaskAddCondition(0, &opened_pred) == 0, "task complete cond");
    check(GameplayTaskEnd() == 0, "task end");

    // 按钮触发器：dist Button Player lt 2 → button.near
    PredicatePod near_pred;
    near_pred.kind = 1; near_pred.op = 0;   // dist, lt
    std::snprintf(near_pred.entity_a, sizeof(near_pred.entity_a), "self");
    std::snprintf(near_pred.entity_b, sizeof(near_pred.entity_b), "Player");
    near_pred.value = 2.f;
    check(GameplayTriggerAdd(button, &near_pred, "button.near", 1) == 0, "button trigger");

    // ---- 120 帧 @1/60：0-40 帧靠近（10→0），40-120 帧远离（0→10）----
    auto set_player_x = [&](float x) {
        const float pos[3]{ x, 0.f, 0.f }, rot[3]{ 0.f, 0.f, 0.f }, scl[3]{ 1.f, 1.f, 1.f };
        SetEntityTransform(player, pos, rot, scl);
    };
    char state_buf[64] = {};
    for (int f = 0; f < 120; ++f) {
        const float x = f < 40 ? 10.f - 0.25f * f : 0.25f * (f - 40);
        set_player_x(x);
        PipelineTick(1.f / 60.f);
        if (f == 45) { GameplayGetState(door, state_buf, sizeof(state_buf)); res.opened = std::string(state_buf) == "open"; }
        if (f == 119) {
            GameplayGetState(door, state_buf, sizeof(state_buf));
            res.closed = std::string(state_buf) == "closed";
            std::uint32_t tstate = 99;
            res.task_completed = GameplayTaskGetState(door, "open_door", &tstate) == 0 && tstate == 1;
        }
    }

    float door_pos3[3] = {};
    check(GetEntityPosition(door, door_pos3) == 1u, "door position readable");
    res.door_y = door_pos3[1];

    const std::uint32_t n_trace = GameplayTraceRecordCount();
    for (std::uint32_t i = 0; i < n_trace; ++i) {
        TraceRecordPod rec{};
        if (GameplayTraceGetRecord(i, &rec) != 0u) break;
        res.trace.push_back(std::to_string(rec.frame) + "|" + std::to_string(rec.kind) + "|" + rec.detail);
    }

    ScriptDestroyForEntity(script);
    DestroyEntity(door);
    DestroyEntity(button);
    DestroyEntity(player);
    return res;
}

}  // namespace

int main()
{
    // Lua 脚本写到临时目录（每轮同一文件；内容固定=确定性输入）。
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "gameplay_e2e";
    std::filesystem::create_directories(tmp);
    const auto lua_path = tmp / "door.lua";
    {
        std::ofstream out(lua_path);
    // 注意：Lua 模块表每类型共享（跨实例可见）——幂等守卫用 game.state.get()
    //（机器态）而非模块变量，否则同类型二次实例带着旧值。
    out << R"LUA(
local M = {}
function M:begin_play()
    bus.on("button.near", function(payload)
        -- C++ 发射的裸字节载荷 → Lua string（"entity=Button"）
        if string.find(payload, "entity=Button", 1, true) then M.seen_button = true end
        if game.state.get() ~= "open" then
            game.state.transition("open")
        end
    end)
    bus.on("door.opened", function(payload)
        entity.set_position(0, 3, -2)
        state.entities[entity_id].opened = 1
    end)
end
function M:update(dt) end
return M
)LUA";
    }
    check(std::filesystem::exists(lua_path), "lua fixture written");
    check(ScriptEnsureSystems() == 1u, "script systems ready");

    const RunResult r1 = run_round(lua_path.string());
    const RunResult r2 = run_round(lua_path.string());

    // ---- 行为断言 ----
    check(r1.opened, "door state=open while near (Lua transition)");
    check(r1.closed, "door auto-closed when far (guard dist gte 5)");
    check(r1.door_y > kDoorOpenY - 0.01f, "door lifted by Lua handler");
    check(r2.opened && r2.closed && r2.door_y > kDoorOpenY - 0.01f, "second round behaves identically");
    check(r1.task_completed, "door task completed via property condition");
    check(r2.task_completed, "second round task completed too");

    // ---- 回放确定性：同输入同帧序同事件流（entity id 含 generation 位，比较
    //      kind+frame+detail）----
    bool identical = r1.trace.size() == r2.trace.size();
    if (identical)
        for (size_t i = 0; i < r1.trace.size(); ++i)
            if (r1.trace[i] != r2.trace[i]) { identical = false; break; }
    check(identical, "replay: trace streams byte-identical");

    // 事件链断言：trigger_fired(button.near) → state_lua(closed→open) →
    //             state_auto(open→closed) 顺序成立
    auto find_idx = [&](const RunResult& r, const char* needle) {
        for (size_t i = 0; i < r.trace.size(); ++i)
            if (r.trace[i].find(needle) != std::string::npos) return (int)i;
        return -1;
    };
    const int i_trig = find_idx(r1, "button.near");
    const int i_open = find_idx(r1, "to=open");
    const int i_close = find_idx(r1, "to=closed");
    check(i_trig >= 0, "trace has button.near");
    check(i_open > i_trig, "open transition after trigger");
    check(i_close > i_open, "auto close after open");
    check(find_idx(r1, "|2|") == i_open, "open transition is state_lua kind");
    check(find_idx(r1, "task=open_door") > i_open, "task completed after open");

    std::cout << (failures ? "[TestGameplayE2E] FAILED\n" : "[TestGameplayE2E] ALL PASS\n");
    return failures ? 1 : 0;
}
