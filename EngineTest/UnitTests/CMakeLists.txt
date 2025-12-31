cmake_minimum_required(VERSION 3.20)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置测试输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests)

# ======================
# 独立RHI测试目标 - 不依赖任何外部库
# ======================

# 创建RHI类型系统测试
add_executable(StandaloneRHI_Test RHI/Core/TestRHITypes.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Test COMMAND StandaloneRHI_Test)
set_target_properties(StandaloneRHI_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Test PRIVATE DEBUG _DEBUG)

# 创建RHI设备管理测试
add_executable(StandaloneRHI_Device_Test RHI/Core/TestRHIDevice.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Device_Test COMMAND StandaloneRHI_Device_Test)
set_target_properties(StandaloneRHI_Device_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Device_Test PRIVATE DEBUG _DEBUG)

# 创建RHI资源管理测试
add_executable(StandaloneRHI_Resource_Test RHI/Core/TestRHIResource.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Resource_Test COMMAND StandaloneRHI_Resource_Test)
set_target_properties(StandaloneRHI_Resource_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Resource_Test PRIVATE DEBUG _DEBUG)

# 创建RHI自适应内存池测试
add_executable(StandaloneRHI_AdaptiveMemoryPool_Test RHI/Core/TestRHIAdaptiveMemoryPool.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_AdaptiveMemoryPool_Test COMMAND StandaloneRHI_AdaptiveMemoryPool_Test)
set_target_properties(StandaloneRHI_AdaptiveMemoryPool_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_AdaptiveMemoryPool_Test PRIVATE DEBUG _DEBUG)

# 创建RHI命令队列测试
add_executable(StandaloneRHI_Queue_Test RHI/Core/TestRHIQueue.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Queue_Test COMMAND StandaloneRHI_Queue_Test)
set_target_properties(StandaloneRHI_Queue_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Queue_Test PRIVATE DEBUG _DEBUG)



# 设置moodycamel库路径
set(MOODYCAMEL_ROOT ${CMAKE_SOURCE_DIR}/third_party/moodycamel-ConcurrentQueue)
set(MOODYCAMEL_INCLUDE_DIRS ${MOODYCAMEL_ROOT})

# MPSC队列简化验证测试 (符合技术规范)
add_executable(MpscQueueSimpleTest MpscQueueSimpleTest.cpp)
# 包含头文件目录
target_include_directories(MpscQueueSimpleTest PRIVATE 
    ${MOODYCAMEL_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 设置编译定义
target_compile_definitions(MpscQueueSimpleTest PRIVATE 
    DEBUG 
    _DEBUG
)
set_target_properties(MpscQueueSimpleTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
# 添加pthread支持
target_link_libraries(MpscQueueSimpleTest PRIVATE pthread)
# 添加到测试集合
add_test(NAME MpscQueueSimpleTest COMMAND MpscQueueSimpleTest)

# ======================
# 创建测试集合目标
# ======================

## 创建一个宏来运行所有RHI测试
add_custom_target(RHI_All_Tests
    COMMAND StandaloneRHI_Test
    COMMAND StandaloneRHI_Device_Test
    COMMAND StandaloneRHI_Resource_Test
    COMMAND StandaloneRHI_AdaptiveMemoryPool_Test
    COMMAND StandaloneRHI_Queue_Test
    COMMAND MpscQueueSimpleTest
    COMMENT "Running all RHI tests"
)

# 为IDE创建文件夹结构
set_target_properties(StandaloneRHI_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Device_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Resource_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Queue_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_AdaptiveMemoryPool_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(MpscQueueSimpleTest PROPERTIES FOLDER "Tests/RHI")
set_target_properties(RHI_All_Tests PROPERTIES FOLDER "Tests/RHI")

# 输出构建信息
message(STATUS "RHI Unit Tests configured:")
message(STATUS "  - StandaloneRHI_Test: Type system tests")
message(STATUS "  - StandaloneRHI_Device_Test: Device management tests") 
message(STATUS "  - StandaloneRHI_Resource_Test: Resource management tests")
message(STATUS "  - StandaloneRHI_Queue_Test: Command queue tests")
message(STATUS "  - RHI_All_Tests: Run all RHI tests")