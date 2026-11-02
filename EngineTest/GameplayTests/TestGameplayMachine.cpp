// TestGameplayMachine.cpp — gameplay S1 单测：状态机构建器校验/自动转移/
// 受控转移语义（gameplay-rules.md S1 验收：非法转移/悬空状态有确定语义）。

#include "Gameplay/GameplayRuntime.h"
#include "Components/ScriptEventBus.h"

#include <iostream>
#include <string>
#include <vector>

using namespace primal;
namespace gprt = primal::gameplay::runtime;
namespace gpsm = primal::script;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestGameplayMachine] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestGameplayMachine] ok: " << name << std::endl;
    }
}

}  // namespace

int main()
{
    gpsm::script_event_bus::instance().reset();
    gprt::clear();

    // ---- 构建器校验：错误码逐项确定语义 ----
    {
        check(gprt::machine_begin(100, "idle") == gameplay::gp_error::ok, "begin ok");
        check(gprt::machine_begin(101, "x") == gameplay::gp_error::busy, "double begin busy");
        check(gprt::machine_add_state("idle") == gameplay::gp_error::ok, "add idle");
        check(gprt::machine_add_state("idle") == gameplay::gp_error::already_exists, "dup state");
        check(gprt::machine_add_state("open") == gameplay::gp_error::ok, "add open");
        check(gprt::machine_add_transition("idle", "nope", nullptr) == gameplay::gp_error::dangling_state,
              "dangling to");
        check(gprt::machine_add_transition("idle", "open", "door.opened") == gameplay::gp_error::ok, "trans ok");
        gameplay::gp_predicate g;
        g.kind = gameplay::predicate_kind::timer;
        g.op = gameplay::compare_op::gte;
        g.value = 1.f;
        check(gprt::machine_add_guard(0, g) == gameplay::gp_error::ok, "guard ok");
        check(gprt::machine_add_guard(9, g) == gameplay::gp_error::bad_index, "guard bad index");
        check(gprt::machine_end() == gameplay::gp_error::ok, "end ok");

        std::string st;
        check(gprt::state_get(100, st) && st == "idle", "initial state");
        check(!gprt::state_get(999, st), "no machine for unknown entity");
    }

    // ---- 初始态未声明：End 校验失败整组丢弃 ----
    {
        gprt::clear();
        gpsm::script_event_bus::instance().reset();
        check(gprt::machine_begin(200, "a") == gameplay::gp_error::ok, "begin 200");
        check(gprt::machine_add_state("b") == gameplay::gp_error::ok, "add b");
        check(gprt::machine_end() == gameplay::gp_error::dangling_state, "initial missing");
        std::string st;
        check(!gprt::state_get(200, st), "discarded machine invisible");
    }

    // ---- 自动转移：timer guard + 声明序优先 + 每 tick 至多一次 ----
    {
        gprt::clear();
        gpsm::script_event_bus::instance().reset();
        std::vector<std::string> events;
        const u64 sub = gpsm::script_event_bus::instance().subscribe_by_name(
            "door.opened", &events,
            [](void* ud, const void*, u64) {
                static_cast<std::vector<std::string>*>(ud)->push_back("door.opened");
            });

        check(gprt::machine_begin(300, "a") == gameplay::gp_error::ok, "begin chain");
        gprt::machine_add_state("a"); gprt::machine_add_state("b"); gprt::machine_add_state("c");
        gprt::machine_add_transition("a", "b", "door.opened");   // guard: timer gte 1
        gprt::machine_add_transition("b", "c", nullptr);         // guard: timer gte 1（下一 tick 才走）
        gameplay::gp_predicate t;
        t.kind = gameplay::predicate_kind::timer; t.op = gameplay::compare_op::gte; t.value = 1.f;
        gprt::machine_add_guard(0, t);
        gprt::machine_add_guard(1, t);
        check(gprt::machine_end() == gameplay::gp_error::ok, "end chain");

        gprt::tick(0.5f);
        std::string st;
        check(gprt::state_get(300, st) && st == "a", "timer below → stay a");
        gprt::tick(0.6f);   // 累计 1.1 ≥ 1 → a→b（本 tick 结束，b→c 不再评估）
        check(gprt::state_get(300, st) && st == "b", "one transition per tick");
        gprt::tick(0.6f);   // b 停留 0.6 <1 → 留在 b
        check(gprt::state_get(300, st) && st == "b", "state timer resets on entry");
        gprt::tick(0.6f);   // b 停留 1.2 ≥ 1 → b→c
        check(gprt::state_get(300, st) && st == "c", "chain b→c");

        gpsm::script_event_bus::instance().drain();
        check(events.size() == 1, "transition event emitted once");
        gpsm::script_event_bus::instance().unsubscribe(sub);
    }

    // ---- guard AND：一条 false 即不转移 ----
    {
        // 不 clear：保留块 3 的 auto trace 记录（尾块做累积断言）。
        check(gprt::machine_begin(400, "x") == gameplay::gp_error::ok, "begin and");
        gprt::machine_add_state("x"); gprt::machine_add_state("y");
        gprt::machine_add_transition("x", "y", nullptr);
        gameplay::gp_predicate t1;
        t1.kind = gameplay::predicate_kind::timer; t1.op = gameplay::compare_op::gte; t1.value = 0.f;
        gameplay::gp_predicate c2;
        c2.kind = gameplay::predicate_kind::event_count; c2.op = gameplay::compare_op::gte; c2.key = "never"; c2.value = 1.f;
        gprt::machine_add_guard(0, t1);
        gprt::machine_add_guard(0, c2);   // 永假（事件从未发生）
        gprt::machine_end();
        gprt::tick(0.1f);
        std::string st;
        check(gprt::state_get(400, st) && st == "x", "AND guard blocks");
    }

    // ---- Lua 受控转移：结构校验（边存在性）；非法转移确定语义 ----
    {
        // 不 clear：保留前序块的 trace 记录（下一块做累积断言）；
        // 本块只操作实体 500，与遗留机器无交互。
        check(gprt::machine_begin(500, "closed") == gameplay::gp_error::ok, "begin lua");
        gprt::machine_add_state("closed"); gprt::machine_add_state("open");
        gprt::machine_add_transition("closed", "open", "door.opened");
        gprt::machine_add_transition("open", "closed", nullptr);
        gprt::machine_end();
        std::string detail;
        check(gprt::state_transition(500, "open", detail) == gameplay::gp_error::ok, "lua transition ok");
        std::string st;
        check(gprt::state_get(500, st) && st == "open", "now open");
        check(gprt::state_transition(500, "closed", detail) == gameplay::gp_error::ok, "back ok");
        check(gprt::state_transition(500, "nirvana", detail) == gameplay::gp_error::no_transition,
              "no edge → no_transition");
        check(detail.find("closed->nirvana") != std::string::npos, "detail names the edge");
        check(gprt::state_transition(777, "x", detail) == gameplay::gp_error::no_machine, "no machine");
    }

    // ---- trace：state_auto/state_lua 记录 + 帧号 ----
    {
        u32 n = gprt::trace_record_count();  // 上一块保留（trace 跨块累积断言）
        bool saw_auto = false, saw_lua = false;
        for (u32 i = 0; i < n; ++i) {
            u32 frame; gameplay::trace_kind kind; u64 entity; std::string det;
            if (!gprt::trace_get(i, frame, kind, entity, det)) { check(false, "trace get"); break; }
            if (kind == gameplay::trace_kind::state_auto) saw_auto = true;
            if (kind == gameplay::trace_kind::state_lua) saw_lua = true;
        }
        check(saw_auto, "trace has auto transitions");
        check(saw_lua, "trace has lua transitions");
        u32 frame; gameplay::trace_kind kind; u64 entity; std::string det;
        check(!gprt::trace_get(n, frame, kind, entity, det), "trace oob returns false");
    }

    gprt::clear();
    gpsm::script_event_bus::instance().reset();
    std::cout << (failures ? "[TestGameplayMachine] FAILED\n" : "[TestGameplayMachine] ALL PASS\n");
    return failures ? 1 : 0;
}
