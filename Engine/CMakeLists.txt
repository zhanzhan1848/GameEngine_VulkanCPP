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
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.mm"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderMesh.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderMesh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderTexture.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderTexture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderScene.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderProxy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderProxy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Material.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Material.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialInstance.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialInstance.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/SceneDataAdapter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/SceneDataAdapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ForwardRenderer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ForwardRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Systems/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Systems/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Utils/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Utils/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Components/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Components/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Passes/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Passes/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Scene/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Volume/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Volume/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Field/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Field/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Fluid/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Fluid/*.cpp"
    # Ensure SSRPass is included (GLOB_RECURSE should pick it up, but touching file forces rescan)
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderGraph/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderGraph/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/Modules/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/Modules/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/Debug/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/Debug/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/PostProcess/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/PostProcess/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/Visibility/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/RenderPasses/Visibility/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lighting/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lighting/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utils/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utils/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.cpp"
    # "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/GraphicsHandle.hpp"
)

# 检查是否启用WebGPU支持
option(ENABLE_WEBGPU "Enable WebGPU support via Dawn library" OFF)

# 检查是否禁用粒子系统
option(DISABLE_PARTICLE_SYSTEM "Disable particle system compilation" OFF)

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
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.h"
    )
elseif(LINUX)
    file(GLOB_RECURSE GRAPHICS_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.cpp"
    )
endif()

# 添加WebGPU平台支持（如果启用）
if(ENABLE_WEBGPU)
    # 设置Dawn库路径
    set(DAWN_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/dawn)
    
    # 检查Dawn库是否存在
    if(EXISTS ${DAWN_ROOT_DIR})
        message(STATUS "Found Dawn library at: ${DAWN_ROOT_DIR}")
        
        # 添加Dawn库作为子项目
        add_subdirectory(${DAWN_ROOT_DIR} third_party/dawn_build)
        
        # 设置Dawn库目标
        set(DAWN_LIBRARIES
            dawn_native
            dawn_proc
            dawn_wire
            dawn_platform
            dawn_common
            dawn_utils
        )
        
        # 设置Dawn包含目录
        set(DAWN_INCLUDE_DIRS
            ${DAWN_ROOT_DIR}/include
            ${DAWN_ROOT_DIR}/src
            ${DAWN_ROOT_DIR}/src/include
            ${DAWN_ROOT_DIR}/gen
        )
        
        # 收集WebGPU平台源文件
        file(GLOB_RECURSE WEBGPU_SOURCES 
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platform/WebGPU/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platform/WebGPU/*.cpp"
        )
        
        # 将WebGPU源文件添加到图形源文件列表
        list(APPEND GRAPHICS_SOURCES ${WEBGPU_SOURCES})
        
        # 添加WebGPU平台定义
        add_definitions(-DENABLE_WEBGPU=1)
        
        message(STATUS "WebGPU platform configured successfully")
    else()
        message(WARNING "Dawn library not found at: ${DAWN_ROOT_DIR}")
        message(WARNING "WebGPU support will be disabled")
        set(ENABLE_WEBGPU OFF)
    endif()
else()
    message(STATUS "WebGPU support disabled")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/JobSystem/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JobSystem/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Particles/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Particles/*.cpp"
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

# 粒子系统可选编译
if(DISABLE_PARTICLE_SYSTEM)
    message(STATUS "Particle system disabled - excluding from build")
    list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/Particles/.*")
endif()

# 合并所有源文件
# Module .cpp files are included in Engine library (used by StandardRenderPipeline)
# list(FILTER GRAPHICS_UTILS EXCLUDE REGEX "Modules/.*\\.cpp$")

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
    
    # 添加项目头文件
    target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Common
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
        ${CMAKE_SOURCE_DIR}/third_party/stb
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

# 粒子系统禁用定义
if(DISABLE_PARTICLE_SYSTEM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DISABLE_PARTICLE_SYSTEM=1)
endif()

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
            # -fno-rtti
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
            # -fno-rtti
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

# === moodycamel::ConcurrentQueue 支持 ===

# 设置moodycamel库路径
set(MOODYCAMEL_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/moodycamel-ConcurrentQueue)

# 检查moodycamel库是否存在
if(EXISTS ${MOODYCAMEL_ROOT_DIR})
    message(STATUS "Found moodycamel ConcurrentQueue at: ${MOODYCAMEL_ROOT_DIR}")
    
    # 设置moodycamel包含目录
    set(MOODYCAMEL_INCLUDE_DIRS ${MOODYCAMEL_ROOT_DIR})
    
    # 添加moodycamel包含目录
    target_include_directories(${PROJECT_NAME} PRIVATE ${MOODYCAMEL_INCLUDE_DIRS})
    
    # 添加moodycamel相关编译定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        ENABLE_MOODYCAMEL_CONCURRENT_QUEUE=1
    )
    
    message(STATUS "moodycamel ConcurrentQueue configured successfully")
else()
    message(WARNING "moodycamel ConcurrentQueue not found at: ${MOODYCAMEL_ROOT_DIR}")
    message(WARNING "MPSC queue performance will be degraded")
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        ENABLE_MOODYCAMEL_CONCURRENT_QUEUE=0
    )
endif()

# 添加WebGPU/Dawn库支持（如果启用）
if(ENABLE_WEBGPU)
    # 添加Dawn包含目录
    target_include_directories(${PROJECT_NAME} PRIVATE ${DAWN_INCLUDE_DIRS})
    
    # 链接Dawn库
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DAWN_LIBRARIES})
    
    # 根据平台添加特定的Dawn依赖
    if(WIN32)
        # Windows特定依赖
        target_link_libraries(${PROJECT_NAME} PRIVATE
            d3d12
            dxgi
            dxguid
        )
    elseif(APPLE)
        # macOS特定依赖
        find_library(METAL_LIBRARY Metal)
        find_library(QUARTZCORE_LIBRARY QuartzCore)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            ${METAL_LIBRARY}
            ${QUARTZCORE_LIBRARY}
        )
    elseif(UNIX)
        # Linux特定依赖
        find_package(X11 REQUIRED)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            ${X11_LIBRARIES}
            pthread
            dl
        )
    endif()
    
    # 添加WebGPU平台定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        PLATFORM_WEBGPU=1
    )
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