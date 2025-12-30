cmake_minimum_required(VERSION 3.20)

# 独立RHI类型系统测试项目
project(StandaloneRHI_Tests CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 创建独立的RHI类型测试可执行文件
add_executable(StandaloneRHI_Test TestRHITypes.cpp)

# 创建独立的RHI设备测试可执行文件
add_executable(StandaloneRHI_Device_Test TestRHIDevice.cpp)

# 创建独立的RHI资源测试可执行文件
add_executable(StandaloneRHI_Resource_Test TestRHIResource.cpp)

# 创建独立的RHI队列测试可执行文件
add_executable(StandaloneRHI_Queue_Test TestRHIQueue.cpp)

# 设置输出目录
set_target_properties(StandaloneRHI_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_target_properties(StandaloneRHI_Device_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_target_properties(StandaloneRHI_Resource_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_target_properties(StandaloneRHI_Queue_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 不依赖任何外部库，完全独立