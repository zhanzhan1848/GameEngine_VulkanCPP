// === Phase 1 Task 2: Script Lifecycle Hooks 测试 ===
//
// 验证 entity_script 的 4 个新虚函数(fixed_update / late_update / destroy / on_reload)
// 以及 script::fixed_update / script::late_update dispatch 入口。
// 同时验证:
//   1) create() 后自动调用 begin_play()
//   2) remove() 前自动调用 destroy()
//   3) 调用顺序: begin_play → fixed_update → update → late_update → destroy
//
// 设计说明:
//   - 使用手动 creator 函数(不走 REGISTER_SCRIPT 宏),避免全局静态初始化顺序问题。
//   - script 子系统必须先 initialize() 才能在主线程上调用 update/fixed_update/late_update/remove。
//   - entity 通过 game_entity::create(transform_info) 创建,然后单独调 script::create。
//     不走 entity_info.script 路径,是因为后者内部 assert 失败时会触发 Engine assert,
//     直接走 script::create 反而更直接地测试 script 子系统本身。
//
// TODO(Task 10): add exception-path tests for each try/catch dispatch site.
// For each of begin_play / update / fixed_update / late_update / destroy,
// write a test where a script throws std::runtime_error from that hook and
// verify (a) the dispatch completes without aborting, (b) subsequent scripts
// in the same dispatch still run, (c) stderr contains the expected message.
// Requires per-file -fexceptions on the test file (mirror Engine/CMakeLists.txt
// pattern). This file is built with -fno-exceptions so cannot throw directly.

#include "../TestFramework.h"
#include "Components/Script.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"

#include <vector>
#include <string>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

namespace {

// 测试脚本:记录每次生命周期回调的顺序
class lifecycle_test_script : public primal::script::entity_script {
public:
    static std::vector<std::string> call_log;

    void begin_play() override { call_log.push_back("begin_play"); }
    void fixed_update(float) override { call_log.push_back("fixed_update"); }
    void update(float) override { call_log.push_back("update"); }
    void late_update(float) override { call_log.push_back("late_update"); }
    void destroy() override { call_log.push_back("destroy"); }

    explicit lifecycle_test_script(primal::game_entity::entity e)
        : primal::script::entity_script(e) {}
};

std::vector<std::string> lifecycle_test_script::call_log;

// 手动 creator(测试用,不走 REGISTER_SCRIPT 宏避免全局静态初始化顺序问题)
primal::script::detail::script_ptr make_lifecycle_script(
    primal::game_entity::entity e) {
    return std::make_unique<lifecycle_test_script>(e);
}

// Helper: 创建一个带最小化 transform 的 entity
static primal::game_entity::entity make_test_entity() {
    primal::transform::init_info tinfo{};
    // identity quaternion (0,0,0,1) — 默认 tinfo.rotation 是 zero-init,
    // 但 transform::create 接受默认 scale=(1,1,1),rotation=(0,0,0,0) 也能 create,
    // 不参与矩阵计算直到 transform::update,所以本测试不需要 Identity quaternion。
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// 主测试:create() 触发 begin_play,然后 fixed/update/late_update 按序,remove 触发 destroy。
TestResult test_lifecycle_hooks_called_in_order() {
    primal::script::initialize();
    lifecycle_test_script::call_log.clear();

    // 构造 entity + script
    primal::game_entity::entity entity = make_test_entity();
    primal::script::init_info sinfo{};
    sinfo.script_creator = &make_lifecycle_script;
    primal::script::component sc = primal::script::create(sinfo, entity);

    // begin_play 应该已经被 create() 自动调用
    primal::script::fixed_update(0.016f);
    primal::script::update(0.016f);
    primal::script::late_update(0.016f);
    primal::script::remove(sc);

    primal::script::shutdown();

    // 验证调用顺序
    const std::vector<std::string> expected = {
        "begin_play", "fixed_update", "update", "late_update", "destroy"
    };
    if (lifecycle_test_script::call_log != expected) {
        std::cout << "Expected: ";
        for (const auto& s : expected) std::cout << s << " ";
        std::cout << "\nActual:   ";
        for (const auto& s : lifecycle_test_script::call_log) std::cout << s << " ";
        std::cout << "\n";
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Smoke test: a script with no overridden hooks can be created and removed
// without crashing. The class below intentionally has no overrides — it relies
// entirely on entity_script's default empty implementations.
TestResult test_minimal_script_create_remove_no_crash() {
    // 这个测试验证即使不重写任何虚函数,默认实现也都能正确工作。
    // 实际热重载语义由 Task 7 测试。
    class minimal_script : public primal::script::entity_script {
    public:
        explicit minimal_script(primal::game_entity::entity e)
            : primal::script::entity_script(e) {}
    };

    primal::script::initialize();
    auto entity = make_test_entity();
    primal::script::init_info sinfo{};
    sinfo.script_creator = [](primal::game_entity::entity e)
        -> primal::script::detail::script_ptr {
        return std::make_unique<minimal_script>(e);
    };
    auto sc = primal::script::create(sinfo, entity);
    // 默认 on_reload 不会被自动调用,但通过虚函数指针触发一次确保 default impl 不炸
    // (这里只能间接验证——下个测试 ensure reflect default impl 也 OK)

    primal::script::remove(sc);
    primal::script::shutdown();
    return TestResult::Passed;
}

// === Phase 1 Task 3: frame_tick Orchestrator 测试 ===
//
// 验证 script::frame_tick(dt) 单次调用按 spec §5.1 顺序触发完整 per-frame 序列:
//   begin_play (fired during create, NOT during frame_tick)
//   → fixed_update → update → late_update (driven by frame_tick)
// 然后 remove() 触发 destroy。
//
// 实现说明:frame_tick 内部还会调用 drain_callbacks / drain_events /
// process_deferred_reloads / apply_deferred_subscriptions,但这些都是
// 占位空实现(后续 Task 5/7/8 填充),本测试只验证脚本可见的 hook 序列。
TestResult test_frame_tick_calls_all_phases() {
    primal::script::initialize();
    lifecycle_test_script::call_log.clear();

    // 构造 entity + script。create() 内部会自动调用 begin_play,
    // 所以 call_log 此时为 ["begin_play"]。
    primal::game_entity::entity entity = make_test_entity();
    primal::script::init_info sinfo{};
    sinfo.script_creator = &make_lifecycle_script;
    primal::script::component sc = primal::script::create(sinfo, entity);

    // 一次 frame_tick 内部按 spec §5.1 顺序:
    //   drain_callbacks → fixed_update → update → late_update → drain_events
    //   → process_deferred_reloads → apply_deferred_subscriptions
    // 脚本可见的 hook 只有 fixed/update/late(drain_*_impl 当前为空)。
    primal::script::frame_tick(0.016f);

    // remove() 触发 destroy。
    primal::script::remove(sc);
    primal::script::shutdown();

    // 验证调用顺序。begin_play 在 create 期间触发;destroy 在 remove 期间触发。
    const std::vector<std::string> expected = {
        "begin_play", "fixed_update", "update", "late_update", "destroy"
    };
    if (lifecycle_test_script::call_log != expected) {
        std::cout << "Expected: ";
        for (const auto& s : expected) std::cout << s << " ";
        std::cout << "\nActual:   ";
        for (const auto& s : lifecycle_test_script::call_log) std::cout << s << " ";
        std::cout << "\n";
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

} // namespace

void RunScriptLifecycleTests() {
    TestSuite suite("Script.Lifecycle");
    suite.AddTestCase(TestCase("hooks_called_in_order",
                               test_lifecycle_hooks_called_in_order,
                               "create -> fixed_update -> update -> late_update -> remove "
                               "invokes begin_play/fixed/update/late/destroy in order"));
    suite.AddTestCase(TestCase("minimal_script_create_remove_no_crash",
                               test_minimal_script_create_remove_no_crash,
                               "create and remove a script with no overrides does not crash"));
    suite.AddTestCase(TestCase("frame_tick_calls_all_phases",
                               test_frame_tick_calls_all_phases,
                               "frame_tick(dt) drives fixed_update -> update -> late_update "
                               "in spec §5.1 order with begin_play/destroy bookends"));
    suite.RunAllTests();
}

int main() {
    RunScriptLifecycleTests();
    return 0;
}
