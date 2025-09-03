# CMake最低版本要求
cmake_minimum_required(VERSION 3.20)

# 设置CMake策略
cmake_policy(SET CMP0116 NEW)
set(CMAKE_DEPENDS_USE_COMPILER FALSE)

# 修复路径处理
set(CMAKE_SUPPRESS_REGENERATION ON)
set(CMAKE_USE_RELATIVE_PATHS ON)
set(CMAKE_UNITY_BUILD OFF)

# 设置生成器相关选项
if(CMAKE_GENERATOR MATCHES "Unix Makefiles")
    set(CMAKE_VERBOSE_MAKEFILE ON)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

# 设置项目名称和版本
project(GameEngine_VulkanCPP
    VERSION 1.0
    DESCRIPTION "跨平台渲染引擎"
    LANGUAGES CXX C
)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_OSX_ARCHITECTURES "arm64")

# 设置构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type: Debug Release" FORCE)
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})

# 定义全局路径变量
set(ENGINE_ASSETS_DIR ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Assets)
set(ENGINE_SHADERS_DIR ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Shaders)
set(ENGINE_RESOURCES_DIR ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Resources)

# 生成配置头文件
configure_file(
    ${CMAKE_SOURCE_DIR}/Engine/Common/EngineConfig.h.in
    ${CMAKE_SOURCE_DIR}/Engine/Common/EngineConfig.h
)

# 导入CMake工具模块
# include(CMake/Default.cmake)
# include(CMake/DefaultCXX.cmake)
# include(CMake/Utils.cmake)

# 全局编译定义
if(WIN32)
    add_definitions(
        -DWIN32
        -D_WINDOWS
        -DUNICODE
        -D_UNICODE
    )
    
    # Windows特定设置
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    
elseif(APPLE)
    add_definitions(
        -DAPPLE
        -DMACOS
    )
    
    # macOS特定设置
    set(CMAKE_MACOSX_RPATH ON)

    # FBX
    set(FBX_SDK_ROOT /Applications/Autodesk/FBX\ SDK/2020.3.7)
    include_directories(${FBX_SDK_ROOT}/include)

    # 添加 FBX SDK 库路径
    link_directories(
        "${FBX_SDK_ROOT}/lib/clang/debug"
        "${FBX_SDK_ROOT}/lib/clang/release"
    )

    file(COPY "${FBX_SDK_ROOT}/lib/clang/debug/libfbxsdk.dylib" DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
    
else()
    add_definitions(
        -DLINUX
    )
    
    # Linux特定设置
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Debug/Release特定定义
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(
        -DDEBUG
        -D_DEBUG
    )
else()
    add_definitions(-DNDEBUG)
endif()

# 设置通用编译选项
if(MSVC)
    # MSVC编译选项
    add_compile_options(
        /MP             # 多处理器编译
        /W4             # 警告级别
        /WX             # 警告视为错误
        /GR-            # 禁用RTTI
        /EHs-c-        # 禁用异常处理
        /fp:fast        # 快速浮点计算
    )
else()
    # GCC/Clang编译选项
    add_compile_options(
        -Wall
        -Wextra
        # -Werror
        -fno-rtti              # 禁用RTTI
        # -fno-exceptions        # 禁用异常处理
        -ffast-math           # 快速浮点计算
        -Wunused-parameter
    )
endif()

# 查找必要的包
# find_package(Vulkan REQUIRED)

# 设置全局包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

# 添加子项目
add_subdirectory(ContentTools)
add_subdirectory(Engine)
add_subdirectory(EngineDLL)
add_subdirectory(EngineTest)

# 设置构建顺序依赖
add_dependencies(ContentTools Engine)
add_dependencies(EngineDLL Engine ContentTools)
add_dependencies(EngineTest Engine ContentTools EngineDLL)

# 配置安装规则
install(TARGETS Engine EngineDLL ContentTools EngineTest
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# 打印配置信息
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator: ${CMAKE_GENERATOR}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
if(WIN32)
    message(STATUS "Windows SDK Version: ${CMAKE_SYSTEM_VERSION}")
endif()
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Vulkan Found: ${Vulkan_FOUND}")
message(STATUS "Output Directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
