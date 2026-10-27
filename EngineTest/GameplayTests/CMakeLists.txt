# GameplayTests — 玩法规则骨架 S1 单测（无 GPU 依赖；前三个链 Engine 静态库，
# E2E 链 EngineDLL 走与 PrimalPlayer 完全相同的 C ABI 路径）。
# 独立子目录注册（NavigationTests 先例——不经 UnitTests/CMakeLists.txt，
# 避免与并行渲染会话的未提交改动混提交）。
foreach(gp_test
    TestGameplayMachine
    TestGameplayPredicates
    TestGameplayTrace
    TestGameplayTask)
    add_executable(${gp_test} ${gp_test}.cpp)
    target_include_directories(${gp_test} PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Engine ${CMAKE_SOURCE_DIR}/Engine/Common)
    target_link_libraries(${gp_test} PRIVATE Engine)
    add_test(NAME Unit${gp_test} COMMAND ${gp_test})
    set_tests_properties(Unit${gp_test} PROPERTIES TIMEOUT 60)
endforeach()

# E2E：靠近按钮 → 触发器事件 → Lua 受控转移 → Lua 挪门 → 远离自动关门；
# 双轮跑比对 trace（同输入同帧序同事件流）。只链 EngineDLL——纯 C ABI 消费者；
# 再链静态 Engine 会双副本单例（transform/gameplay 运行态读到空副本，实测断言）。
add_executable(TestGameplayE2E TestGameplayE2E.cpp)
target_link_libraries(TestGameplayE2E PRIVATE EngineDLL)
add_test(NAME UnitTestGameplayE2E COMMAND TestGameplayE2E)
set_tests_properties(UnitTestGameplayE2E PROPERTIES TIMEOUT 120)
