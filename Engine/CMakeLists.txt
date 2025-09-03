cmake_minimum_required(VERSION 3.20)

# 项目名称
set(PROJECT_NAME Engine)
project(${PROJECT_NAME})


# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE MACOSX_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
set(CMAKE_OSX_SYSROOT "${MACOSX_PREFIX}")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS")

# 根据平台分别收集源文件
file(GLOB_RECURSE PLATFORM_UTILS
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/Window.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/Window.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/Platform.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/PlatformTypes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Platform/IncludeWindowCPP.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Input/Input.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Input/Input.cpp"
)
if(WIN32)
    file(GLOB_RECURSE PLATFORM_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/PlatformWin32.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Input/InputWin32.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Input/InputWin32.cpp"
    )
elseif(APPLE)
    file(GLOB_RECURSE PLATFORM_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/PlatformMac.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Input/InputMac.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Input/InputMac.mm"
    )
elseif(LINUX)
    file(GLOB_RECURSE PLATFORM_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/PlatformLinux.cpp"
    )
endif()

# 根据平台分别收集图形部分代码文件
file(GLOB_RECURSE GRAPHICS_UTILS
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/GraphicsPlatform.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/GraphicsPlatform.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/GraphicsPlatformInterface.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Renderer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Renderer.cpp"
    # "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/GraphicsHandle.hpp"
)
if(WIN32)
    file(GLOB_RECURSE GRAPHICS_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Direct3D12/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Direct3D12/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.cpp"
    )
elseif(APPLE)
    file(GLOB_RECURSE GRAPHICS_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Metal/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Metal/*.cpp"
    )
elseif(LINUX)
    file(GLOB_RECURSE GRAPHICS_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.cpp"
    )
endif()

# 收集通用源文件（排除平台特定文件）
file(GLOB_RECURSE COMMON_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Components/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Components/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineAPI/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineAPI/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.cpp"
)

# 根据编译器排除特定文件
if(MSVC)
    # MSVC编译器特定的源文件
    file(GLOB_RECURSE COMPILER_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/MSVC/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/MSVC/*.h"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang编译器特定的源文件
    file(GLOB_RECURSE COMPILER_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/Clang/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/Clang/*.h"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC编译器特定的源文件
    file(GLOB_RECURSE COMPILER_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/GCC/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/GCC/*.h"
    )
endif()

# 手动排除特定文件（如果需要）
list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*_test\\.cpp$")  # 排除测试文件
# list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/Platform/.*")   # 排除所有平台相关文件

# 合并所有源文件
set(ALL_SOURCES
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
    ${COMPILER_SOURCES}
    ${PLATFORM_UTILS}
    ${GRAPHICS_UTILS}
    ${GRAPHICS_SOURCES}
)

# 创建静态库
add_library(${PROJECT_NAME} STATIC ${ALL_SOURCES})

# 禁用某些可能导致问题的功能
set_target_properties(${PROJECT_NAME} PROPERTIES
    SKIP_BUILD_RPATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
)

# 设置完整的包含路径
get_target_property(ENGINE_SOURCE_DIR ${PROJECT_NAME} SOURCE_DIR)

# 添加包含目录
if(WIN32)
    target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Common
        $ENV{VULKAN_SDK}/include
    )
elseif(APPLE)
    
    # 设置LLVM编译器
    # set(CMAKE_CXX_COMPILER /opt/homebrew/Cellar/llvm/20.1.5/bin/clang++)
    
    # 设置编译器标志
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++17")
    
    # 设置Objective-C++标志
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -std=c++17")
    
    # 设置链接器标志
    # set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/opt/homebrew/Cellar/llvm/20.1.5/lib -Wl,-rpath,/opt/homebrew/Cellar/llvm/20.1.5/lib")
    
    # 添加系统头文件目录（顺序很重要）
    include_directories(SYSTEM BEFORE
        # /opt/homebrew/Cellar/llvm/20.1.5/include/c++/v1
        # /opt/homebrew/Cellar/llvm/20.1.5/lib/clang/19/include
        ${MACOSX_PREFIX}/usr/include
        ${MACOSX_PREFIX}/System/Library/Frameworks
    )
    
    # 添加项目头文件
    target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Common
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    
    # 添加编译器选项
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-deprecated-declarations
        -Wno-unused-parameter
        -Wno-c++17-extensions
        -Wignored-qualifiers
    )
endif()

# 编译定义
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        DEBUG 
        _DEBUG
    )
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NDEBUG)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PRIMAL_PLUS
    _LIB
    _HAS_EXCEPTIONS=0
)

# 编译选项
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /MP
        /GS-
        /fp:fast
        /GR-
        /Y-
    )
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${PROJECT_NAME} PRIVATE /Od /ZI)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE /Ox /Oi /GF /Oy /Gy)
    endif()
    
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(${PROJECT_NAME} PRIVATE /OPT:REF /OPT:ICF)
    endif()
    
    # Windows平台需要D3D12
    find_package(Microsoft.Direct3D.D3D12 CONFIG)
    if(Microsoft.Direct3D.D3D12_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE Microsoft::Direct3D-D3D12)
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall
            -Wextra
            -ffast-math
            -fno-exceptions
            -fno-rtti
            -fno-strict-aliasing  # 放宽别名规则
            -msse4.2              # 启用SSE4.2指令集
            -mavx                 # 启用AVX指令集
            -Wno-ignored-attributes  # 忽略特定属性警告
            -faligned-new        # 确保new操作符使用对齐内存
        )
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall
            -Wextra
            -ffast-math
            -fno-exceptions
            -fno-rtti
            -fvisibility=hidden
        )
    endif()
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${PROJECT_NAME} PRIVATE -O0 -g)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    endif()
endif()

# 根据平台添加编译定义
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32
        _WINDOWS
        SOLUTION_DIR="${CMAKE_SOURCE_DIR}/"
    )
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        __APPLE__
    )
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        __linux__
    )
endif()

# Vulkan支持
# find_package(Vulkan REQUIRED)
# target_link_libraries(${PROJECT_NAME} PRIVATE
#     Vulkan::Vulkan
# )

# 添加平台相关库
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        X11
        dl
        pthread
    )
elseif(APPLE)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(METAL_LIBRARY Metal)
    find_library(METALKIT_LIBRARY MetalKit)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${FOUNDATION_LIBRARY}
        ${COCOA_LIBRARY}
        ${METAL_LIBRARY}
        ${METALKIT_LIBRARY}
        ${QUARTZCORE_LIBRARY}
    )

    # find_library(ACCELERATE Accelerate)
    # target_link_libraries(${PROJECT_NAME} PRIVATE
    #     ${ACCELERATE}
    # )
    
endif()

# 输出目录
# set_target_properties(${PROJECT_NAME} PROPERTIES
#     ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}"
#     LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}"
# )

# 源文件分组 (方便在IDE中查看)
source_group(TREE ${CMAKE_SOURCE_DIR} PREFIX "Source" FILES ${ALL_SOURCES})

# 添加编译器定义
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        COMPILER_MSVC=1
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        COMPILER_CLANG=1
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        COMPILER_GCC=1
    )
endif()

# 添加平台定义（虽然已经有内置的定义，但有时候可能需要自定义）
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        PLATFORM_WINDOWS=1
    )
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        PLATFORM_MAC=1
    )
elseif(UNIX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        PLATFORM_LINUX=1
    )
endif()