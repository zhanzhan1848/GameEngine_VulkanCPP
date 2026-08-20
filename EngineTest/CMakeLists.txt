# 设置项目名称
set(PROJECT_NAME EngineTest)

# CMake最低版本要求
cmake_minimum_required(VERSION 3.20)

# 设置编译器标志（-stdlib=libc++ 仅 Apple/clang；Linux 下会把 g++/clang++
# 指向未安装的 libc++ 导致 'stdlib.h' file not found）
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++17")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
endif()
    
# 设置Objective-C++标志
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -std=c++17")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS")

# 包含子目录
add_subdirectory(UnitTests)
add_subdirectory(IntegrationTests)
# Lua backend test requires third_party/lua submodule sources. Skip if missing
# (e.g. offline environments where submodule cannot be cloned).
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/lua/lapi.c")
    add_subdirectory(IntegrationTests/ScriptLuaBackend)
else()
    message(WARNING "third_party/lua/lapi.c not found — skipping ScriptLuaBackend tests (submodule not initialized).")
endif()

# 创建自定义目标来运行所有测试
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS RHI_All_Tests IntegrationTests
    COMMENT "Running all tests (Unit Tests + Integration Tests)"
)

# 创建自定义目标来运行单元测试
add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R RHI_All_Tests
    DEPENDS RHI_All_Tests
    COMMENT "Running Unit Tests only"
)

# 创建自定义目标来运行集成测试
add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R Engine_Integration_Tests
    DEPENDS IntegrationTests
    COMMENT "Running Integration Tests only"
)

# 设置CTEST配置
# NOTE: enable_testing() 已上移至顶层 CMakeLists.txt（必须在 add_subdirectory
# 之前调用才能让本目录树内的 add_test 注册进 ctest）。
set(CTEST_OUTPUT_ON_FAILURE TRUE)

# 输出配置信息
message(STATUS "Engine Test Suite configured successfully")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Available test targets:")
message(STATUS "  - run_all_tests: Run both unit and integration tests")
message(STATUS "  - run_unit_tests: Run unit tests only") 
message(STATUS "  - run_integration_tests: Run integration tests only")