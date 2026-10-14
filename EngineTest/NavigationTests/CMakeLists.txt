# NavigationTests — ai-navigation S1 单测（无 GPU 依赖，直接链 Engine 静态库）。
# 独立子目录注册（不经 UnitTests/CMakeLists.txt，避免与并行渲染会话的未提交改动混提交）。
foreach(nav_test
    TestNavigationBake
    TestNavigationPath
    TestNavigationSerialize
    TestNavigationRuntime)
    add_executable(${nav_test} ${nav_test}.cpp)
    target_include_directories(${nav_test} PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Engine ${CMAKE_SOURCE_DIR}/Engine/Common)
    target_link_libraries(${nav_test} PRIVATE Engine)
    add_test(NAME Unit${nav_test} COMMAND ${nav_test})
endforeach()
