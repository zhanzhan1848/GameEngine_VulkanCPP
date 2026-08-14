#include "../TestFramework.h"
#include "Components/Script.h"

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using namespace primal::script;

// TODO(Task 10): add a negative test that spawns a worker thread, calls
// script::update(0.0f) on it after initialize(), and verifies the assert
// fires. Requires fork()/signal handling — defer to Task 10 regression suite.

namespace {
// 主线程上调用 initialize → update → shutdown,内部 check_main_thread 不 assert 即通过。
TestResult test_initialize_sets_main_thread_id() {
    primal::script::initialize();
    // initialize 后内部 check_main_thread() 在主线程上不应 assert。
    primal::script::update(0.0f);
    primal::script::shutdown();
    return TestResult::Passed;
}

// 主线程上 update 不应触发主线程断言。
TestResult test_main_thread_check_passes_on_main() {
    primal::script::initialize();
    // 多次 update 确保稳定。
    primal::script::update(0.0f);
    primal::script::update(0.016f);
    primal::script::shutdown();
    return TestResult::Passed;
}

// initialize/shutdown 可重复调用(后续 task 可能要求 idempotent,这里只做基础回归)。
TestResult test_initialize_shutdown_is_repeatable() {
    primal::script::initialize();
    primal::script::update(0.0f);
    primal::script::shutdown();
    primal::script::initialize();
    primal::script::update(0.0f);
    primal::script::shutdown();
    return TestResult::Passed;
}
} // namespace

void RunScriptFoundationTests() {
    TestSuite suite("Script.Foundation");
    suite.AddTestCase(TestCase("initialize_sets_main_thread_id",
                               test_initialize_sets_main_thread_id,
                               "initialize captures main thread id; update passes"));
    suite.AddTestCase(TestCase("main_thread_check_passes_on_main",
                               test_main_thread_check_passes_on_main,
                               "check_main_thread does not assert on the main thread"));
    suite.AddTestCase(TestCase("initialize_shutdown_is_repeatable",
                               test_initialize_shutdown_is_repeatable,
                               "initialize/shutdown can be called more than once"));
    suite.RunAllTests();
}

int main() {
    RunScriptFoundationTests();
    return 0;
}
