cmake_minimum_required(VERSION 3.20)

# 项目名称
set(PROJECT_NAME Engine)
project(${PROJECT_NAME})


# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE AND NOT EMSCRIPTEN)
    execute_process(
            COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE MACOSX_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    set(CMAKE_OSX_SYSROOT "${MACOSX_PREFIX}")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS")
endif()

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
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/*.mm"
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
elseif(EMSCRIPTEN)
    file(GLOB_RECURSE PLATFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/PlatformEmscripten.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/StubsEmscripten.cpp"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Camera/RenderViewBuilder.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Camera/RenderViewBuilder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderProxy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderProxy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Material.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Material.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialInstance.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialInstance.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ParticleMaterial.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ParticleMaterial.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/SceneDataAdapter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/SceneDataAdapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ForwardRenderer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ForwardRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/VirtualTexture/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/VirtualTexture/*.cpp"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/DebugDraw/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/DebugDraw/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/Providers/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Nanite/Providers/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Volume/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Volume/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Field/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Field/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Fluid/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Fluid/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/Nodes/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/Nodes/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/Noise/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/Noise/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/GPU/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/PCG/GPU/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/Nodes/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/Nodes/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/Styles/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/Styles/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialGraph/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialGraph/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialGraph/Nodes/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ShaderIR/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/ShaderIR/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialPreview/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/MaterialPreview/*.cpp"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Neural/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Neural/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Baking/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Baking/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utils/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utils/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/*.cpp"
    # "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/GraphicsHandle.hpp"
)

# 检查是否启用WebGPU支持
option(ENABLE_WEBGPU "Enable WebGPU support via Dawn library" OFF)

# 检查是否启用 Vulkan RHI 后端 (Win32 + Linux + macOS via MoltenVK)
option(ENABLE_VULKAN "Enable Vulkan RHI backend (Win32 + Linux + macOS via MoltenVK)" OFF)

# D3D12 RHI 后端（仅 Windows；可与 ENABLE_VULKAN 同 ON —— 双后端单一测试二进制，
# 运行时按 DeviceDesc.platform 分发）。macOS/Linux 上此 option 被忽略：
# 只编平台中立核白名单（见下方 D3D12 分层注释）。
option(ENABLE_D3D12 "Enable D3D12 RHI backend (Windows only)" OFF)
# Agility SDK 锁版（M0 冻结，R6 工具版本漂移风险）。历史 premake5.lua 用的
# 1.608.2 与 props 1.606.3 混用已废弃；统一锁 1.611.x 单一版本。
# 2026-11-03 修正：1.611.3 在 nuget.org 不存在（flatcontainer 404，实测），
# 1.611.x 实际可用止于 1.611.2 —— 锁 1.611.2。
set(PRIMAL_D3D12_AGILITY_VERSION "1.611.2" CACHE STRING
    "Microsoft.Direct3D.D3D12 (Agility SDK) version used when ENABLE_D3D12=ON")

# 检查是否禁用粒子系统
option(DISABLE_PARTICLE_SYSTEM "Disable particle system compilation" OFF)

if(WIN32)
    if(ENABLE_VULKAN OR ENABLE_D3D12)
        # ENABLE_VULKAN 或 ENABLE_D3D12=ON 时只编自包含的新 RHI 后端
        # （RHI/Platforms/{Vulkan,D3D12}）。legacy Direct3D12 / Graphics/Vulkan
        # 基于旧 XMFLOAT 数学层（XMLoad/XMStore 直取 math 类型地址），与新的
        # 带运算符数学类型不兼容；macOS 的 ENABLE_VULKAN 构建同样不包含这两
        # 个目录。ENABLE_D3D12 同样排除 legacy Direct3D12（F0-3）。
        set(GRAPHICS_SOURCES "")
    else()
        file(GLOB_RECURSE GRAPHICS_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Direct3D12/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Direct3D12/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.cpp"
        )
    endif()
elseif(APPLE)
    file(GLOB_RECURSE GRAPHICS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Metal/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Metal/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Metal/*.h"
    )
elseif(LINUX)
    if(ENABLE_VULKAN)
        set(GRAPHICS_SOURCES "")
    else()
        file(GLOB_RECURSE GRAPHICS_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Vulkan/*.cpp"
        )
    endif()
endif()

# Vulkan RHI 后端 (Engine/Graphics/RHI/Platforms/Vulkan/*) — 与平台独立的 legacy Graphics/Vulkan/* 解耦。
# ENABLE_VULKAN=ON 时编入；legacy Graphics/Vulkan/* 在 Win32/Linux 仍按上面的 glob 编入（不同 namespace、不同抽象层）。
if(ENABLE_VULKAN)
    file(GLOB_RECURSE VULKAN_RHI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Vulkan/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Vulkan/*.cpp"
    )
    list(APPEND GRAPHICS_SOURCES ${VULKAN_RHI_SOURCES})
    message(STATUS "Vulkan RHI backend enabled (Engine/Graphics/RHI/Platforms/Vulkan)")
endif()

# D3D12 RHI 后端 (Engine/Graphics/RHI/Platforms/D3D12/*) — M0 分层隔离：
#   1) 平台中立核（无任何 Windows 头，macOS 可编译+单测）：
#      Abi / Format / StateMapping / StateTracker / RootSignatureBudget。
#      这五个文件在【所有平台】编入（显式白名单，不 glob——防止未来误把
#      Windows 专属文件带进 macOS 树）。
#   2) Windows 专属 COM 层（D3D12Device/Fence/Buffer/Texture/SwapChain/
#      Pipeline/CommandBuffer/DescriptorHeaps，include d3d12.h 家族）：
#      仅 WIN32 AND ENABLE_D3D12 时 glob 全目录编入 + ENABLE_D3D12=1 宏 +
#      Agility SDK REQUIRED（F0-2/F0-3）。
if(WIN32 AND ENABLE_D3D12)
    file(GLOB_RECURSE D3D12_RHI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/*.cpp"
    )
    list(APPEND GRAPHICS_SOURCES ${D3D12_RHI_SOURCES})
    # F0-4: D3D12MemAlloc v3.0.1（third_party，版本+SHA 见
    # third_party/D3D12MemAlloc/VENDOR.md）。仅 Windows 树编入——
    # src/D3D12MemAlloc.cpp 直接 include <d3d12.h>/<dxgi.h>，进 macOS
    # 编译必然失败；非 Windows 分支（下方 else）不引用本目录。
    list(APPEND GRAPHICS_SOURCES
        "${CMAKE_SOURCE_DIR}/third_party/D3D12MemAlloc/src/D3D12MemAlloc.cpp")
    message(STATUS "D3D12 RHI backend enabled (Engine/Graphics/RHI/Platforms/D3D12, Agility SDK ${PRIMAL_D3D12_AGILITY_VERSION}, D3D12MemAlloc v3.0.1)")
else()
    file(GLOB D3D12_NEUTRAL_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12Abi.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12Format.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12Format.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12StateMapping.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12StateMapping.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12StateTracker.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12StateTracker.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12RootSignatureBudget.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/D3D12/D3D12RootSignatureBudget.cpp"
    )
    list(APPEND GRAPHICS_SOURCES ${D3D12_NEUTRAL_SOURCES})
    if(NOT WIN32)
        message(STATUS "D3D12 RHI neutral core only (Windows-only COM layer excluded; ENABLE_D3D12=${ENABLE_D3D12} ignored on this platform)")
    endif()
endif()

# 添加WebGPU平台支持（如果启用）
if(ENABLE_WEBGPU)
    if(EMSCRIPTEN)
        # Emscripten: use browser's native WebGPU, don't compile Dawn
        file(GLOB_RECURSE WEBGPU_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Dawn/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Dawn/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Dawn/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Dawn/*.cpp"
        )
        list(APPEND GRAPHICS_SOURCES ${WEBGPU_SOURCES})
        add_definitions(-DENABLE_WEBGPU=1)
        message(STATUS "WebGPU platform configured for Emscripten (browser WebGPU)")
    else()
        # Native: compile Dawn from source
        set(DAWN_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/dawn)

        # 检查Dawn库是否存在
        if(EXISTS ${DAWN_ROOT_DIR})
        message(STATUS "Found Dawn library at: ${DAWN_ROOT_DIR}")
        
        # 添加Dawn库作为子项目
        add_subdirectory(${DAWN_ROOT_DIR} third_party/dawn_build)
        
        # 设置Dawn库目标
        set(DAWN_LIBRARIES
            webgpu_dawn
            dawn_native
            dawn_proc
            dawn_wire
            dawn_platform
            dawn_common
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
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Dawn/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Dawn/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Dawn/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RHI/Platforms/Dawn/*.cpp"
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
    endif() # closes if(EMSCRIPTEN) else()
else()
    message(STATUS "WebGPU support disabled")
endif()

# 收集通用源文件（排除平台特定文件）
file(GLOB_RECURSE COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Components/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Components/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Scripting/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Scripting/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Audio/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Audio/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineAPI/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineAPI/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utilities/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Geometry/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Geometry/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Geometry/Algorithms/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JobSystem/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JobSystem/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Simulation/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Simulation/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Particles/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Particles/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ParticleFX/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ParticleFX/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Navigation/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Navigation/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Gameplay/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Gameplay/*.cpp"
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
    list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/ParticlePass.*")
    list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*Particle\\.")
    list(FILTER GRAPHICS_SOURCES EXCLUDE REGEX ".*/ParticlePass.*")
    list(FILTER GRAPHICS_UTILS EXCLUDE REGEX ".*/ParticlePass.*")
endif()

# Emscripten: exclude ContentToEngine (which has gpu_id_from_fake_pointer /
# sizeof(long)>sizeof(int) assertions that fail under WASM). Lumen DDGI is
# now Dawn-compatible (#ifdef __EMSCRIPTEN__ guards on Metal-only paths).
# Nanite IS compiled for WASM — its Metal includes are guarded by
# #ifdef __APPLE__ and ENABLE_WEBGPU paths are implemented.
# StubsEmscripten.cpp provides the ContentToEngine symbols Nanite references.
if(EMSCRIPTEN)
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "Content/ContentToEngine\\.cpp")
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
elseif(APPLE AND NOT EMSCRIPTEN)

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

elseif(EMSCRIPTEN)

    target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Common
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/stb
    )
endif()

# 编译定义
# 必须用生成器表达式：VS/Xcode 多配置生成器下顶层 CMake 会把 CMAKE_BUILD_TYPE
# 视为空（或被强制为 Debug），任何 CMAKE_BUILD_TYPE 判断都会把 Debug 分支应用到
# 全部配置——曾导致 Engine 的 Release 也被注入 _DEBUG：MSVC 的
# _ITERATOR_DEBUG_LEVEL/RuntimeLibrary 标记随 _DEBUG 翻转（MDd/IDL=2），
# 测试（MD/IDL=0）链接 LNK2038 + __imp__calloc_dbg LNK2019。
# NDEBUG 排除 DebugEditor（2026-10-12）：顶层把 Jolt 归一化按 Debug 编
# （无 NDEBUG → JPH_DEBUG/JPH_ENABLE_ASSERTS 开），Engine 消费者 TU 若带
# NDEBUG 则 JPH_DEBUG 关——同一份 Jolt 头两套条件编译，JPH 内联状态与库
# 不一致 → PhysicsInitialize 堆损坏（realloc 非法指针，build-wt 实测
# DebugEditor 必崩/Debug 干净；见 Docs/completed/2026-10-11-scripting-s1-*.md）。
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug,DebugEditor>>:NDEBUG>
)

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
        /GR             # RTTI 必须开：RenderGraph/Nanite/Script 的 dynamic_cast
                        # 在 /GR- 下运行时抛 bad_cast（异常已禁 → 0xc0000409）
        /Y-
    )
    
    # 同上：生成器表达式替代 CMAKE_BUILD_TYPE 判断（/ZI 的 EDITANDCONTINUE
    # 对象曾随 Debug 分支泄漏进 Release，链接期 LNK4075）
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/ZI>
        $<$<NOT:$<CONFIG:Debug>>:/Ox;/Oi;/GF;/Oy;/Gy>
    )

    # 2026-11-03 R2（Windows 首编）：LumenDDGIPass.cpp 在 MSVC 下触发 C1001
    # （内部编译器错误；用户附件未传到——无精确错误行号与配置名）。静态扫描
    # 无明确高危构造（无结构化绑定/std::visit/巨型本地数组；requires 均为
    # 日志字符串），按 MSVC 官方建议对该文件降优化兜底：per-source /Od 追加
    # 在 target 选项之后（MSVC 同组优化选项后者生效，非 Debug 的 /Ox 被压住；
    # Debug 本就 /Od）。待拿到完整 C1001 输出（文件:行号+编译器版本+配置）后
    # 根治并移除本条。仅 MSVC 生效；clang/gcc（macOS/Linux 树）不受影响。
    #
    # 2026-11-03 R3（第三轮，Release 仍 C1001 后的加固）：
    #   a) 属性读回验证——configure 输出必须出现下述 STATUS 行（含 /Od），
    #      否则属性未生效（路径不匹配或被后续同属性调用替换——本文件无其它
    #      set_source_files_properties 触及此源）。
    #   b) /Od 只压后端（c2）；若 Debug 配置也 ICE = 前端（c1）问题，/Od 无效
    #      ——转拆分路线：R3 已把 IO 辅助拆出 LumenDDGIPassIO.cpp（4834→4584
    #      行）；仍复发则继续按块拆 AddPass（~1780 行单函数）。
    #   c) 诊断需要用户提供：C1001 完整输出（文件:行号+触发函数+cl 版本
    #      `cl /B1` 号）+ 配置名（Debug/Release）——见 runbook §11.3。
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/DDGI/LumenDDGIPass.cpp"
        PROPERTIES COMPILE_OPTIONS "/Od")
    get_source_file_property(PRIMAL_DDGI_ICE_OD_FLAGS
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Lumen/DDGI/LumenDDGIPass.cpp" COMPILE_OPTIONS)
    message(STATUS "C1001 mitigation: LumenDDGIPass.cpp per-source COMPILE_OPTIONS='${PRIMAL_DDGI_ICE_OD_FLAGS}' (expect '/Od'; empty = property NOT applied)")

    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF>
    )
    
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
    
    # 生成器表达式替代 CMAKE_BUILD_TYPE 判断（多配置生成器下该变量不可靠）
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
        $<$<NOT:$<CONFIG:Debug>>:-O3>
    )
endif()

# === Phase 1 Task 2: script dispatch 需要在边界 catch 脚本抛出的异常 ===
# (spec 2026-07-07-script-core-capabilities-design.md §6.4)
# Engine 整体 -fno-exceptions,但 Script.cpp 的 fixed_update/late_update/begin_play/destroy
# 必须 catch + log + continue,防止单个脚本异常中断整个 dispatch。Per-file override。
if(NOT MSVC)
    # NOTE: 用 COMPILE_FLAGS 而非 COMPILE_OPTIONS。CMake 4.x 的 Unix Makefiles
    # 生成器对 source-file 级 COMPILE_OPTIONS 在 -fno-exceptions 已由 target 级
    # PRIVATE 选项设定时不会追加(实际编译命令仅含 $(CXX_FLAGS),override 丢失,
    # 导致 Script.cpp 的 try/catch 报 "cannot use 'try' with exceptions disabled")。
    # COMPILE_FLAGS 是 per-file 字符串属性,生成器会去重后追加到该文件命令末尾,
    # -fexceptions 出现在 -fno-exceptions 之后由 clang last-wins 解析生效。
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Components/Script.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Components/ScriptState.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Components/ScriptFilesystem.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripting/LuaBackend.cpp"
        PROPERTIES COMPILE_FLAGS "-fexceptions"
    )
endif()

# === scripting S1（2026-10-11）：Lua 后端生产化 ===
# lua_static 在 Engine 定义（EngineTest/ScriptLuaBackend 有 if(NOT TARGET) 守卫的
# 同款定义，先到先得）。生产目标缺 lua 源是硬错误——与测试树"submodule 缺失可
# 跳过"的策略不同，静默跳过会让脚本链路无声消失（scripting S1 契约）。
if(NOT TARGET lua_static)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/lapi.c")
        message(FATAL_ERROR
            "third_party/lua is missing — Engine now requires Lua 5.4 for the "
            "production script backend (git submodule update --init third_party/lua).")
    endif()
    set(PRIMAL_LUA_SRC
        ../third_party/lua/lapi.c ../third_party/lua/lcode.c ../third_party/lua/lctype.c
        ../third_party/lua/ldebug.c ../third_party/lua/ldo.c ../third_party/lua/ldump.c
        ../third_party/lua/lfunc.c ../third_party/lua/lgc.c ../third_party/lua/llex.c
        ../third_party/lua/lmem.c ../third_party/lua/lobject.c ../third_party/lua/lopcodes.c
        ../third_party/lua/lparser.c ../third_party/lua/lstate.c ../third_party/lua/lstring.c
        ../third_party/lua/ltable.c ../third_party/lua/ltm.c ../third_party/lua/lundump.c
        ../third_party/lua/lvm.c ../third_party/lua/lzio.c ../third_party/lua/lauxlib.c
        ../third_party/lua/lbaselib.c ../third_party/lua/lcorolib.c ../third_party/lua/ldblib.c
        ../third_party/lua/liolib.c ../third_party/lua/linit.c ../third_party/lua/lmathlib.c
        ../third_party/lua/loadlib.c ../third_party/lua/loslib.c ../third_party/lua/lstrlib.c
        ../third_party/lua/ltablib.c ../third_party/lua/lutf8lib.c)
    add_library(lua_static STATIC ${PRIMAL_LUA_SRC})
    target_include_directories(lua_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua)
    if(APPLE)
        target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX=1)
    elseif(WIN32)
        target_compile_definitions(lua_static PRIVATE LUA_USE_WINDOWS=1)
    else()
        target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX=1)
    endif()
endif()
# 静态库传递：最终消费者（EngineDLL/Player/测试）经 PUBLIC 继承 lua 头与符号。
target_link_libraries(${PROJECT_NAME} PUBLIC lua_static)

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

# Vulkan RHI 后端依赖：Vulkan SDK + VulkanMemoryAllocator（单头）
if(ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    # Ubuntu 的 libvulkan-dev 头文件位于 /usr/include，CMake 会把导入目标的
    # include 目录标记为 SYSTEM，生成 -isystem /usr/include —— 显式指向
    # /usr/include 会打乱 g++/clang 的 include_next 链（cstdlib → stdlib.h
    # 断裂，报 'stdlib.h' file not found）。关闭 SYSTEM 改用普通 -I。
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
        set_target_properties(Vulkan::Vulkan PROPERTIES SYSTEM OFF)
    else()
        set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
    endif()
    # /usr/local/include 在 macOS 上是 CMake 的 implicit include directory,会被自动剥离;
    # 显式从 implicit 列表移除,确保 target_include_directories 能把它传给编译器。
    list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIR})
    list(REMOVE_ITEM CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${Vulkan_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ENABLE_VULKAN=1
    )
    if(APPLE)
        # MoltenVK needs Metal/QuartzCore/IOKit frameworks transitively.
        target_link_options(${PROJECT_NAME} PRIVATE
            -framework Metal -framework QuartzCore -framework IOKit
        )
    endif()
    message(STATUS "Vulkan SDK found: ${Vulkan_INCLUDE_DIR}")
endif()

# D3D12 RHI 后端依赖：Agility SDK（NuGet Microsoft.Direct3D.D3D12）。
# 2026-11-03 接入方式修正（Windows 首编 R1）：官方 Agility NuGet 包不提供
# CMake CONFIG（find_package CONFIG REQUIRED 必然失败），改为**零机器特定
# 配置**的动态获取链，优先级降级：
#   1) 用户缓存变量 PRIMAL_D3D12_AGILITY_SDK_DIR（指向含 build/native/include
#      的解压目录，或直接含 d3d12.h 的 include 目录）→ 直接采用；
#   2) 否则 configure 期从 nuget.org flatcontainer 自动下载 pinned nupkg
#      （就是 zip）到 ${CMAKE_BINARY_DIR}/agility-sdk/，file(ARCHIVE_EXTRACT)
#      解压，include 用 build/native/include/，bin 用 build/native/bin/x64/；
#      SHA256 校验（PRIMAL_D3D12_AGILITY_SHA，默认=1.611.2 实测值，来自
#      api.nuget.org 2026-11-03 下载测量；置空=跳过校验并 WARNING）；
#   3) 下载失败（离线）→ 回退 Windows SDK 自带 d3d12.h + WARNING（运行时
#      特性面受 OS/SDK 版本限制），宏 PRIMAL_D3D12_AGILITY_RUNTIME=0；
#   4) Agility 在场 → 宏 PRIMAL_D3D12_AGILITY_RUNTIME=1，并把
#      D3D12Core.dll / d3d12SDKLayers.dll copy_if_different 到共享运行输出
#      根（Windows/<Config>/D3D12/，对齐官方 NuGet targets 的 .\D3D12\
#      约定——D3D12 运行时按 exe 相对路径解析该子目录）。
# 上方 MSVC 分支内保留的软 find_package 只服务 legacy Direct3D12
# （ENABLE_D3D12=OFF 时 legacy 仍可能编入；同样不会找到 CONFIG，无害）。
if(ENABLE_D3D12)
    if(NOT WIN32)
        message(WARNING "ENABLE_D3D12=ON is ignored on non-Windows platforms (D3D12 needs Agility SDK/d3d12.h)")
    else()
        set(PRIMAL_D3D12_AGILITY_SDK_DIR "" CACHE PATH
            "Agility SDK root (contains build/native/include) or a directory directly containing d3d12.h; empty = auto-download from nuget.org")
        set(PRIMAL_D3D12_AGILITY_SHA "414858c5cf25e43022938ab992d78a9639b1a9a48efe5b4a05fca2df9294388c" CACHE STRING
            "SHA256 of the pinned Agility nupkg (measured from api.nuget.org 2026-11-03, 1.611.2); empty = skip verification with WARNING")

        set(PRIMAL_D3D12_AGILITY_INCLUDE_DIR "")
        set(PRIMAL_D3D12_AGILITY_BIN_DIR "")

        if(PRIMAL_D3D12_AGILITY_SDK_DIR)
            # 优先级 1：用户提供的 SDK 目录（解压的 nupkg 根或裸 include 目录）。
            if(EXISTS "${PRIMAL_D3D12_AGILITY_SDK_DIR}/d3d12.h")
                set(PRIMAL_D3D12_AGILITY_INCLUDE_DIR "${PRIMAL_D3D12_AGILITY_SDK_DIR}")
            elseif(EXISTS "${PRIMAL_D3D12_AGILITY_SDK_DIR}/build/native/include/d3d12.h")
                set(PRIMAL_D3D12_AGILITY_INCLUDE_DIR
                    "${PRIMAL_D3D12_AGILITY_SDK_DIR}/build/native/include")
                if(EXISTS "${PRIMAL_D3D12_AGILITY_SDK_DIR}/build/native/bin/x64")
                    set(PRIMAL_D3D12_AGILITY_BIN_DIR
                        "${PRIMAL_D3D12_AGILITY_SDK_DIR}/build/native/bin/x64")
                endif()
            else()
                message(FATAL_ERROR "PRIMAL_D3D12_AGILITY_SDK_DIR='${PRIMAL_D3D12_AGILITY_SDK_DIR}' contains neither d3d12.h nor build/native/include/d3d12.h")
            endif()
        else()
            # 优先级 2：configure 期自动下载 pinned nupkg（stamp 幂等；换版本重下）。
            set(PRIMAL_D3D12_AGILITY_ROOT "${CMAKE_BINARY_DIR}/agility-sdk")
            set(PRIMAL_D3D12_AGILITY_STAMP
                "${PRIMAL_D3D12_AGILITY_ROOT}/extracted/.primal-agility-${PRIMAL_D3D12_AGILITY_VERSION}")
            if(NOT EXISTS "${PRIMAL_D3D12_AGILITY_STAMP}")
                set(PRIMAL_D3D12_AGILITY_NUPKG
                    "${PRIMAL_D3D12_AGILITY_ROOT}/microsoft.direct3d.d3d12.${PRIMAL_D3D12_AGILITY_VERSION}.nupkg")
                file(MAKE_DIRECTORY "${PRIMAL_D3D12_AGILITY_ROOT}")
                message(STATUS "Downloading Agility SDK (Microsoft.Direct3D.D3D12) ${PRIMAL_D3D12_AGILITY_VERSION} from nuget.org ...")
                file(DOWNLOAD
                    "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${PRIMAL_D3D12_AGILITY_VERSION}/microsoft.direct3d.d3d12.${PRIMAL_D3D12_AGILITY_VERSION}.nupkg"
                    "${PRIMAL_D3D12_AGILITY_NUPKG}"
                    TIMEOUT 300
                    STATUS PRIMAL_D3D12_AGILITY_DL_STATUS)
                list(GET PRIMAL_D3D12_AGILITY_DL_STATUS 0 PRIMAL_D3D12_AGILITY_DL_CODE)
                if(PRIMAL_D3D12_AGILITY_DL_CODE EQUAL 0)
                    if(PRIMAL_D3D12_AGILITY_SHA)
                        file(SHA256 "${PRIMAL_D3D12_AGILITY_NUPKG}" PRIMAL_D3D12_AGILITY_DL_SHA)
                        if(NOT PRIMAL_D3D12_AGILITY_DL_SHA STREQUAL "${PRIMAL_D3D12_AGILITY_SHA}")
                            message(FATAL_ERROR "Agility nupkg SHA256 mismatch: got '${PRIMAL_D3D12_AGILITY_DL_SHA}', expected '${PRIMAL_D3D12_AGILITY_SHA}' (pin or investigate)")
                        endif()
                    else()
                        message(WARNING "PRIMAL_D3D12_AGILITY_SHA is empty — skipping nupkg integrity check for ${PRIMAL_D3D12_AGILITY_VERSION}")
                    endif()
                    file(REMOVE_RECURSE "${PRIMAL_D3D12_AGILITY_ROOT}/extracted")
                    file(ARCHIVE_EXTRACT INPUT "${PRIMAL_D3D12_AGILITY_NUPKG}"
                         DESTINATION "${PRIMAL_D3D12_AGILITY_ROOT}/extracted")
                    file(WRITE "${PRIMAL_D3D12_AGILITY_STAMP}"
                         "${PRIMAL_D3D12_AGILITY_VERSION} ${PRIMAL_D3D12_AGILITY_SHA}\n")
                else()
                    list(GET PRIMAL_D3D12_AGILITY_DL_STATUS 1 PRIMAL_D3D12_AGILITY_DL_MSG)
                    message(WARNING "Agility SDK ${PRIMAL_D3D12_AGILITY_VERSION} download failed (${PRIMAL_D3D12_AGILITY_DL_MSG}); falling back to Windows SDK d3d12.h (runtime feature level limited by OS/SDK)")
                endif()
            endif()
            if(EXISTS "${PRIMAL_D3D12_AGILITY_ROOT}/extracted/build/native/include/d3d12.h")
                set(PRIMAL_D3D12_AGILITY_INCLUDE_DIR
                    "${PRIMAL_D3D12_AGILITY_ROOT}/extracted/build/native/include")
                if(EXISTS "${PRIMAL_D3D12_AGILITY_ROOT}/extracted/build/native/bin/x64")
                    set(PRIMAL_D3D12_AGILITY_BIN_DIR
                        "${PRIMAL_D3D12_AGILITY_ROOT}/extracted/build/native/bin/x64")
                endif()
            endif()
        endif()

        target_compile_definitions(${PROJECT_NAME} PRIVATE
            ENABLE_D3D12=1
            PRIMAL_D3D12_AGILITY_VERSION="${PRIMAL_D3D12_AGILITY_VERSION}"
        )
        # F0-4: D3D12MemAlloc v3.0.1 头文件路径（源文件已加入
        # GRAPHICS_SOURCES，见上方 D3D12 块；版本+SHA 见
        # third_party/D3D12MemAlloc/VENDOR.md）。
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${CMAKE_SOURCE_DIR}/third_party/D3D12MemAlloc/include")

        if(PRIMAL_D3D12_AGILITY_INCLUDE_DIR)
            # Agility 头必须排在 Windows SDK 默认 include 之前（BEFORE），
            # 否则 VS 默认 INCLUDE 路径里的老 d3d12.h 仍会被优先解析。
            target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
                "${PRIMAL_D3D12_AGILITY_INCLUDE_DIR}")
            target_compile_definitions(${PROJECT_NAME} PRIVATE
                PRIMAL_D3D12_AGILITY_RUNTIME=1
            )
            # 运行时 DLL 暂存到共享运行输出根的 D3D12/ 子目录（EngineDLL、
            # 全部集成测试 exe 与编辑器加载路径共用该根；官方 NuGet targets
            # 同样复制到 .\D3D12\ 而非 exe 平级）。
            if(PRIMAL_D3D12_AGILITY_BIN_DIR)
                foreach(PRIMAL_D3D12_AGILITY_DLL D3D12Core.dll d3d12SDKLayers.dll)
                    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E make_directory
                            "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/D3D12"
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${PRIMAL_D3D12_AGILITY_BIN_DIR}/${PRIMAL_D3D12_AGILITY_DLL}"
                            "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/D3D12/${PRIMAL_D3D12_AGILITY_DLL}"
                        VERBATIM)
                endforeach()
            endif()
            message(STATUS "Agility SDK (Microsoft.Direct3D.D3D12) ${PRIMAL_D3D12_AGILITY_VERSION} active: include='${PRIMAL_D3D12_AGILITY_INCLUDE_DIR}' runtime DLLs='${PRIMAL_D3D12_AGILITY_BIN_DIR}'")
        else()
            # 优先级 3：离线回退——Windows SDK 自带 d3d12.h（要求 SDK >=
            # 10.0.16299：D3D12_FEATURE_D3D12_OPTIONS3=21 与本代码面所用
            # EnumAdapterByGpuPreference 均在该版本就位；VS2022 自带 SDK
            # 远高于此下限）。能力查询全部运行时 CheckFeatureSupport，
            # 不存在编译期能力绑定。
            target_compile_definitions(${PROJECT_NAME} PRIVATE
                PRIMAL_D3D12_AGILITY_RUNTIME=0
            )
            message(WARNING "D3D12 backend builds against Windows SDK d3d12.h (PRIMAL_D3D12_AGILITY_RUNTIME=0): newest Agility-only API surface and the D3D12/ runtime DLL side-load are unavailable in this configuration")
        endif()
    endif()
endif()

# === Jolt Physics（物理内核；Jolt 头文件只允许出现在 Engine/Simulation/ 内部）===
if(TARGET Jolt)
    target_link_libraries(${PROJECT_NAME} PRIVATE Jolt)
    # 1) Jolt 以 -fno-rtti 编译（不产出 typeinfo）。派生 JPH::DebugRenderer 的 TU 若带
    #    RTTI，其 vtable/typeinfo 会引用基类 typeinfo 导致链接失败。
    # 2) Simulation 各 TU 内联了大量 Jolt 头文件模板/内联代码，引擎全局的 -ffast-math
    #    会在 -O3 下对其实施可能改变语义的浮点变换（build-wt 的 DebugEditor 树上实证
    #    出堆损坏）——对 Simulation 源文件统一屏蔽 fast-math。
    #    注意：同一文件的 set_source_files_properties 是替换语义，必须一次给全。
    file(GLOB PRIMAL_SIMULATION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Simulation/*.cpp")
    set_source_files_properties(${PRIMAL_SIMULATION_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-fast-math")
    if(MSVC)
        set_source_files_properties(Simulation/SimulationDebugDraw.cpp PROPERTIES
            COMPILE_OPTIONS "/GR-")
    else()
        set_source_files_properties(Simulation/SimulationDebugDraw.cpp PROPERTIES
            COMPILE_OPTIONS "-fno-rtti;-fno-fast-math")
    endif()
endif()

# 添加平台相关库
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        X11
        dl
        pthread
    )
elseif(APPLE AND NOT EMSCRIPTEN)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(METAL_LIBRARY Metal)
    find_library(METALKIT_LIBRARY MetalKit)
    find_library(METALFX_LIBRARY MetalFX)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_package(ZLIB REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${FOUNDATION_LIBRARY}
        ${COCOA_LIBRARY}
        ${METAL_LIBRARY}
        ${METALKIT_LIBRARY}
        ${METALFX_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ZLIB::ZLIB
    )

    # find_library(ACCELERATE Accelerate)
    # target_link_libraries(${PROJECT_NAME} PRIVATE
    #     ${ACCELERATE}
    # )

elseif(WIN32)
    # R5（Windows 四测后续）：Windows 无系统 zlib——消费根 CMakeLists WIN32 段
    # FetchContent 拉取的 zlib 1.3.1（zlibstatic，经 primal_zlib_static ALIAS 保
    # 下游目标名不变）。include 路径经 zlibstatic 的 PUBLIC 接口传递（source dir
    # + binary dir 生成的 zconf.h），不再手挂 third_party 路径（vendor 副本已删）。
    # 非 WIN32 分支（上方 APPLE 的 find_package(ZLIB)）逐字不变。
    if(TARGET primal_zlib_static)
        target_link_libraries(${PROJECT_NAME} PRIVATE primal_zlib_static)
    else()
        message(FATAL_ERROR "Engine(WIN32): primal_zlib_static target missing — root CMakeLists zlib FetchContent block did not run")
    endif()
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
    if(EMSCRIPTEN)
        # Emscripten: link browser WebGPU via emdawnwebgpu port
        target_compile_options(${PROJECT_NAME} PUBLIC "--use-port=emdawnwebgpu")
        target_link_options(${PROJECT_NAME} PUBLIC "--use-port=emdawnwebgpu" -sALLOW_MEMORY_GROWTH=1)
    else()
        # 添加Dawn包含目录
        target_include_directories(${PROJECT_NAME} PRIVATE ${DAWN_INCLUDE_DIRS})

        # 链接Dawn库
        target_link_libraries(${PROJECT_NAME} PRIVATE ${DAWN_LIBRARIES})
    
    # 根据平台添加特定的Dawn依赖
    if(NOT EMSCRIPTEN)
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
    endif() # closes if(NOT EMSCRIPTEN)

    # 添加WebGPU平台定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PLATFORM_WEBGPU=1
    )
    endif() # closes if(EMSCRIPTEN) else() for linking
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
# Bounded opt-in cache allocations fail back to the original Gather. Keep the
# exception island local, following Script.cpp's established compile override.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/Utilities/GeometryBVHStore.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Graphics/RenderPipeline/Modules/InstanceNormalCache.cpp"
        PROPERTIES COMPILE_FLAGS "-fexceptions")
endif()
