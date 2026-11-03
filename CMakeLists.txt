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

# macOS deployment target and architecture MUST be set before project()
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# 设置项目名称和版本
project(GameEngine_VulkanCPP
    VERSION 1.0
    DESCRIPTION "跨平台渲染引擎"
    LANGUAGES CXX C
)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# Emscripten WASM build support
if(EMSCRIPTEN)
    add_definitions(-D__EMSCRIPTEN__)
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
endif()
# 设置构建类型
# 仅单配置生成器（Makefile/Ninja）需要默认值。VS/Xcode 多配置生成器不使用
# CMAKE_BUILD_TYPE（恒为空）——此前无条件强制写 Debug 会误导所有依赖该变量的
# 判断（Engine 曾因此在 Release 配置注入 _DEBUG，链接期 MDd/MD 混链 LNK2038）。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type: Debug Release" FORCE)
endif()

# 设置输出目录
if(EMSCRIPTEN)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
elseif(WIN32)
    # VS 多配置下 CMAKE_BUILD_TYPE 为空，原 Windows// 会让 Debug/Release
    # 共写同一个 Engine.lib——任何一次 Debug 构建就会用 MDd 库污染后续
    # Release 链接（LNK2038）。按配置分目录彻底隔离。
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/$<CONFIG>)
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE})
endif()

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
    
elseif(APPLE AND NOT EMSCRIPTEN)
    add_definitions(
        -DAPPLE
        -DMACOS
    )

    # macOS特定设置
    set(CMAKE_MACOSX_RPATH ON)

    # Metal shader validation (xcrun compile check)
    include(CMake/MetalShaderValidation.cmake)
    # Build engine shaders metallib and legacy packed blob for
    # Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders.
    include(CMake/EngineShaderBlob.cmake)

    # FBX SDK 自动检测和安装
    set(FBX_SDK_VERSION "2020.3.7")
    set(FBX_SDK_ROOT "/Applications/Autodesk/FBX SDK/${FBX_SDK_VERSION}")
    set(FBX_SDK_FOUND FALSE)
    
    # 检查多个可能的FBX SDK安装路径
    set(FBX_SDK_POSSIBLE_PATHS
        "${CMAKE_SOURCE_DIR}/third_party/FBX_SDK/Mac"
        "${FBX_SDK_ROOT}"
        "/Applications/Autodesk/FBX SDK/2020.3.7"
        "/Applications/Autodesk/FBX SDK/2020.3.4"
        "/Applications/Autodesk/FBX SDK/2020.3.2"
        "/Applications/Autodesk/FBX SDK/2020.3.1"
        "/Applications/Autodesk/FBX SDK/2020.0.1"
        "/usr/local/fbx-sdk"
        "/opt/fbx-sdk"
    )
    
    # 检查FBX SDK是否已安装
    foreach(PATH ${FBX_SDK_POSSIBLE_PATHS})
        if(EXISTS "${PATH}/include/fbxsdk.h")
            message(STATUS "Found FBX SDK at: ${PATH}")
            set(FBX_SDK_ROOT "${PATH}")
            set(FBX_SDK_FOUND TRUE)
            break()
        endif()
    endforeach()

    # Vendored 副本缺失时，从系统 Autodesk 安装自动落到 third_party/FBX_SDK/Mac，
    # 让新 checkout / worktree 无需手工拷贝即可配置构建。只拷 include/ 与两份
    # dylib 树（约 50MB）。该副本不进版本库（专有 SDK），丢失时由此自动重建。
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/FBX_SDK/Mac/lib/debug/libfbxsdk.dylib"
       AND EXISTS "${FBX_SDK_ROOT}/include/fbxsdk.h"
       AND EXISTS "${FBX_SDK_ROOT}/lib/clang/debug/libfbxsdk.dylib")
        set(_FBX_VENDOR "${CMAKE_SOURCE_DIR}/third_party/FBX_SDK/Mac")
        file(COPY "${FBX_SDK_ROOT}/include/" DESTINATION "${_FBX_VENDOR}/include")
        file(COPY "${FBX_SDK_ROOT}/lib/clang/debug/" DESTINATION "${_FBX_VENDOR}/lib/debug")
        file(COPY "${FBX_SDK_ROOT}/lib/clang/release/" DESTINATION "${_FBX_VENDOR}/lib/release")
        message(STATUS "FBX SDK: vendored ${FBX_SDK_VERSION} into third_party/FBX_SDK/Mac (from ${FBX_SDK_ROOT})")
        # 之后统一以 vendored 副本为唯一解析位置，探测/链接路径保持一致。
        set(FBX_SDK_ROOT "${_FBX_VENDOR}")
    endif()

    # 检查 submodule 是否已初始化
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src/meshoptimizer.h")
        message(FATAL_ERROR
            "Git submodules not initialized!\n"
            "Please run the following command to initialize submodules:\n"
            "    git submodule update --init --recursive"
        )
    endif()

    # === 物理内核选型结论见 Docs/completed/2026-09-11-physics-simulation-survey.md ===
    # PhysX 4.1 已从 third_party/PhysX 归档（2021 冻结分支、Apple Silicon 无官方支持），
    # 改用 Jolt Physics（C++17、MIT、macOS ARM64 官方支持），接入代码在 add_subdirectory(Engine) 之前。
    
    # 如果没有找到FBX SDK，提供安装指导
    if(NOT FBX_SDK_FOUND)
        message(WARNING "FBX SDK not found in any standard location.")
        message(STATUS "FBX SDK is required for FBX file support.")
        message(STATUS "Please install FBX SDK manually using one of the following methods:")
        message(STATUS "1. Download from Autodesk Developer Network:")
        message(STATUS "   https://aps.autodesk.com/developer/overview/fbx-sdk")
        message(STATUS "   Choose 'Download SDK for mac' and install to /Applications/Autodesk/FBX SDK/")
        message(STATUS "")
        message(STATUS "2. Using Homebrew (if available):")
        message(STATUS "   brew install --cask fbx-sdk")
        message(STATUS "")
        message(STATUS "3. Manual installation:")
        message(STATUS "   Download the macOS Clang (Universal Binary) package")
        message(STATUS "   Extract to: ${FBX_SDK_ROOT}")
        message(STATUS "")
        message(STATUS "After installation, re-run CMake to detect FBX SDK.")
        
        # 可选：自动下载（需要用户确认）
        option(AUTO_DOWNLOAD_FBX_SDK "Automatically download FBX SDK if not found" OFF)
        if(AUTO_DOWNLOAD_FBX_SDK)
            message(STATUS "Attempting to download FBX SDK automatically...")
            
            # 创建外部项目来下载FBX SDK
            include(ExternalProject)
            
            # 设置下载和安装目录
            set(FBX_SDK_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/fbx-sdk")
            
            # 创建FBX SDK安装目录
            file(MAKE_DIRECTORY "/Applications/Autodesk/FBX SDK")
            
            # 配置外部项目下载FBX SDK（使用Autodesk官方下载页面）
            ExternalProject_Add(fbx_sdk_download
                URL "https://aps.autodesk.com/fbx-sdk"
                DOWNLOAD_DIR "${FBX_SDK_DOWNLOAD_DIR}"
                SOURCE_DIR "${FBX_SDK_DOWNLOAD_DIR}/fbx-sdk-source"
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND ""
                BUILD_COMMAND ""
                INSTALL_COMMAND ""
                LOG_DOWNLOAD ON
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
                # 由于需要登录下载，自动下载可能失败
            )
            
            message(WARNING "Automatic download requires Autodesk account login.")
            message(WARNING "Please download manually from the Autodesk website.")
        endif()
        
        # 设置FBX SDK为未找到
        set(FBX_SDK_FOUND FALSE)
    endif()
    
    # 如果FBX SDK找到，配置包含目录和库
    if(FBX_SDK_FOUND)
        # 包含FBX SDK头文件
        include_directories("${FBX_SDK_ROOT}/include")
        
        # 添加FBX SDK库路径
        if(EXISTS "${FBX_SDK_ROOT}/lib/debug")
            link_directories("${FBX_SDK_ROOT}/lib/debug")
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/debug")
            link_directories("${FBX_SDK_ROOT}/lib/debug")
        endif()

        if(EXISTS "${FBX_SDK_ROOT}/lib/release")
            link_directories("${FBX_SDK_ROOT}/lib/release")
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/release")
            link_directories("${FBX_SDK_ROOT}/lib/release")
        endif()
        
        # 复制FBX SDK动态库到输出目录
        if(EXISTS "${FBX_SDK_ROOT}/lib/debug/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/debug/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/debug/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/debug/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/release/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/release/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/release/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/release/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        endif()
        
        # 添加FBX SDK编译定义
        add_definitions(-DENABLE_FBX_SDK=1)
        
        message(STATUS "FBX SDK configured successfully at: ${FBX_SDK_ROOT}")
    else()
        message(STATUS "FBX SDK support disabled - Install FBX SDK to enable FBX file support")
        add_definitions(-DENABLE_FBX_SDK=0)
    endif()
    
else()
    add_definitions(
        -DLINUX
    )
    
    # Linux特定设置
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Debug/Release特定定义
# 必须用生成器表达式而非 CMAKE_BUILD_TYPE 判断：VS 等多配置生成器的
# CMAKE_BUILD_TYPE 恒为空（上文还会将其强制设为 Debug），导致 Release 配置
# 也被注入 _DEBUG，链接期混入 Debug CRT 符号（LNK2019 __imp__CrtDbgReport）。
add_compile_definitions(
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:DebugEditor>:DEBUG>
    $<$<CONFIG:DebugEditor>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:ReleaseEditor>:NDEBUG>
    $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)

# 设置通用编译选项
if(MSVC)
    # 显式钉死运行时库选择：Release=MD / Debug=MDd。曾出现 Engine 的 Release
    # 以 MDd 编译导致测试链接 LNK2038 混链（环境/缓存来源不明），显式声明
    # 消除一切歧义。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # 双保险：命令行显式 /MD——cl 以最后出现的 /M 标志为准，可覆盖任何
    # 来源注入到 vcxproj RuntimeLibrary 属性的 MDd
    add_compile_options($<$<CONFIG:Release>:/MD> $<$<CONFIG:Debug>:/MDd>)
    # MSVC编译选项
    add_compile_options(
        /MP             # 多处理器编译
        /W4             # 警告级别
        /utf-8          # 源码与执行字符集均为 UTF-8：源文件含中文注释/字符串，
                        # 默认按系统代码页(GBK)解读会产生 C4819/C2001 大量误报
        /GR             # 显式开启 RTTI：Script/Nanite/RenderGraph 等 12 处
                        # dynamic_cast 依赖运行时类型信息。/GR- 下 MSVC 的
                        # dynamic_cast 运行时抛 bad_cast，异常又被 /EHs-c- 禁用
                        # → terminate → abort(0xc0000409)。GCC/Clang 侧
                        # -fno-rtti 一直处于注释状态（RTTI 开启），MSVC 对齐。
        /EHs-c-        # 禁用异常处理
        /fp:fast        # 快速浮点计算
        # /WX 暂不启用（与 clang 侧一致，-Werror 本就注释）：
        # Windows 首次全量编译存在结构性警告（C4324 对齐填充、C4100 未引用参数等），
        # 先以 /W4 观察输出，清零后再恢复警告即错误。
    )
    # windows.h 的 min/max 宏与 std::min/std::max 及 AABB 等成员名冲突
    # （C2059/C2589/C4003 大面积级联），全局屏蔽
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # GCC/Clang编译选项
    add_compile_options(
        -Wall
        -Wextra
        # -Werror
        # -fno-rtti              # 禁用RTTI
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
    ${CMAKE_SOURCE_DIR}/third_party/stb
)
if(APPLE)
    # OSAPI/MAC 仅 macOS；此前 NOT EMSCRIPTEN 守卫把 Objective-C 头文件
    # 目录泄漏给了 Windows/Linux 的 include 路径
    include_directories(${CMAKE_SOURCE_DIR}/OSAPI/MAC)
endif()

# 启用测试注册（必须在所有 add_subdirectory 之前，否则子目录里的 add_test
# 会被静默丢弃——EngineTest 曾因在其自身 CMakeLists 的 add_subdirectory 之后
# 才调用 enable_testing() 导致 ctest 永远列出 0 个测试）
enable_testing()

# 添加子项目
# ContentTools 的 Windows 构建依赖 FBX SDK 与 DirectXTex（find_package REQUIRED，
# 且仓库内没有对应的 Find 模块），无这些 SDK 的机器（如 CI runner）用
# -DBUILD_CONTENT_TOOLS=OFF 跳过；Engine 与 RHI 单元测试不依赖 ContentTools。
option(BUILD_CONTENT_TOOLS "Build ContentTools (Windows requires FBX SDK + DirectXTex)" ON)
# VT M1: 跨平台烘焙 CLI + astc-encoder 静态库(vtbake 与 VTEX 单测共用;
# 与 BUILD_CONTENT_TOOLS 解耦 — 本机(macOS)ContentTools 主工程因 FBX SDK
# 不可配置,但 vtbake 必须可构建可验证)
option(BUILD_VTBAKE "Build vtbake CLI (VT offline baker, needs zlib)" ON)
# R5：zlib 1.3.1 改为 FetchContent git 拉取（用户要求源码不进 git 仓库传递；
# R4 的 third_party/zlib vendor 副本已删除，来源/实测记录迁至
# Docs/RHI/D3D12_Windows_Integration_Runbook.md）。Windows 无系统 zlib——用户日志
# ForwardSceneRenderer.cpp(55) 与 VirtualTextureSystem.cpp(16) 双 C1083
# （zlib.h 打不开），且 vtbake/CLI/VT 测试的 find_package(ZLIB) 在 Windows 无从
# 命中。定义放在所有消费者（vtbake → Engine → PrimalEngineCli → EngineTest）
# add_subdirectory 之前。非 WIN32 零影响（各平台 find_package 现状不变）。
if(WIN32)
    include(FetchContent)
    # v1.3.1 锁定 commit（git ls-remote refs/tags/v1.3.1^{} 实测 2026-11-03，
    # 与 R4 tarball 逐文件 diff 交叉验证一致；tag 对象 925af44f）。
    set(PRIMAL_ZLIB_GIT_SHA "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf")
    # 离线/本地覆盖（对齐 Agility 链 PRIMAL_D3D12_AGILITY_VERSION 风格）：
    # 指向含 CMakeLists.txt 的本地 zlib 源目录（如用户已有 clone），存在则跳过网络。
    set(PRIMAL_ZLIB_SOURCE_DIR "" CACHE PATH
        "Local zlib source directory (must contain CMakeLists.txt) used instead of git fetch")
    # zlib 1.3.1 自带 CMakeLists 选项面（本机读其源码实测）：唯一 option 是
    # ZLIB_BUILD_EXAMPLES；**不存在** ZLIB_BUILD_SHARED_LIBS/ZLIB_BUILD_STATIC_LIBS
    # （那是 zlib-ng/后续版本的变量）——zlib(SHARED) 与 zlibstatic(STATIC) 两目标
    # 无条件同时定义。这里关掉 examples，链接 zlibstatic（经 ALIAS 保下游目标名），
    # 并把无人消费的 SHARED zlib 目标排除出 ALL（被排除的目标仍可被依赖构建）。
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON)  # 普通变量：抑制子项目 install 规则（zlib CMakeLists 实测）
    if(PRIMAL_ZLIB_SOURCE_DIR)
        if(NOT EXISTS "${PRIMAL_ZLIB_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "PRIMAL_ZLIB_SOURCE_DIR='${PRIMAL_ZLIB_SOURCE_DIR}' has no CMakeLists.txt — "
                "point it at a zlib 1.3.1 source checkout or unset it to git-fetch "
                "${PRIMAL_ZLIB_GIT_SHA} from https://github.com/madler/zlib.git")
        endif()
        FetchContent_Declare(zlib SOURCE_DIR "${PRIMAL_ZLIB_SOURCE_DIR}")
    else()
        FetchContent_Declare(zlib
            GIT_REPOSITORY https://github.com/madler/zlib.git
            GIT_TAG ${PRIMAL_ZLIB_GIT_SHA})
    endif()
    FetchContent_MakeAvailable(zlib)
    if(NOT TARGET zlibstatic)
        message(FATAL_ERROR "Primal: zlib FetchContent produced no zlibstatic target")
    endif()
    add_library(primal_zlib_static ALIAS zlibstatic)
    set_property(TARGET zlib PROPERTY EXCLUDE_FROM_ALL TRUE)
    # zconf.h 由 zlib 的 CMakeLists configure_file 生成在 binary dir；zlibstatic 的
    # PUBLIC include 已含 source+binary 两目录，消费者仅链接即得（无需手挂路径）。
    if(PRIMAL_ZLIB_SOURCE_DIR)
        set(PRIMAL_ZLIB_ORIGIN "local-dir")
    else()
        set(PRIMAL_ZLIB_ORIGIN "git-fetch")
    endif()
    message(STATUS "Primal: zlib 1.3.1 static (source: ${PRIMAL_ZLIB_ORIGIN}, commit ${PRIMAL_ZLIB_GIT_SHA}, dir ${zlib_SOURCE_DIR})")
endif()
if(NOT EMSCRIPTEN AND BUILD_VTBAKE)
    # 走 astc-encoder 根 CMakeLists(负责声明/翻译 ISA 选项),单 ISA native;
    # 关 WERROR 避免第三方警告在 CI 高版本编译器下变 fatal
    set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
    set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
    set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
    set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
    set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
    set(ASTCENC_ISA_NATIVE ON CACHE BOOL "" FORCE)
    set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/astc-encoder astc-encoder-vt)
    add_subdirectory(ContentTools/vtbake)
endif()

if(NOT EMSCRIPTEN AND BUILD_CONTENT_TOOLS)
    add_subdirectory(ContentTools)
endif()
# === Jolt Physics（Engine/Simulation/ 的物理内核，third_party/JoltPhysics submodule）===
# 选项必须在 add_subdirectory 之前设置。入口是 Build/ 子目录（仓库根没有 CMakeLists.txt）。
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)          # 避免拉 gtest
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)   # Debug 树保干净；release LTO 另议
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE) # 与引擎动态 CRT 一致，避免 Win 侧运行时混链
# DEBUG_RENDERER_IN_DEBUG_AND_RELEASE 默认 ON：Debug 配置带 JPH_DEBUG_RENDERER，供 SimulationDebugDraw 使用
# 静态链接：libJolt.a 进 libEngine.a，避开 dylib 导出/typeinfo 问题（引擎此前的 dylib 导出坑同理）
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Jolt 的条件编译（JPH_ENABLE_ASSERTS/JPH_DEBUG_RENDERER/NDEBUG 等）只认标准 config 名。
# 仓库自定义 build type（如 build-wt 的 DebugEditor）不命中其 $<CONFIG:...> gen-expr 时，
# libJolt.a 与消费方头文件内联代码会条件编译不一致 → 类布局错位 → 堆损坏。
# 在 Jolt 子目录范围内归一化为 Debug（PUBLIC 宏会传播给 Engine 保持两边一致）。
set(JOLT_ORIG_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|ReleaseASAN|ReleaseUBSAN|ReleaseTSAN|ReleaseCoverage|Distribution)$")
    set(CMAKE_BUILD_TYPE Debug)
endif()
add_subdirectory(third_party/JoltPhysics/Build)
set(CMAKE_BUILD_TYPE "${JOLT_ORIG_BUILD_TYPE}")
if(NOT JOLT_ORIG_BUILD_TYPE MATCHES "^(Debug|Release|ReleaseASAN|ReleaseUBSAN|ReleaseTSAN|ReleaseCoverage|Distribution)$")
    # Jolt 的 INTERFACE 条件定义按"消费者"的 config 求值：非标准 config（如 DebugEditor）
    # 永远不命中 gen-expr，收不到这些宏 → 与 libJolt.a 条件编译不一致（布局错位）。
    # 这里把 Jolt Debug 语义下会生效的条件宏显式展开为无条件（对 Jolt 本体是无害重复）。
    # 若未来漂移，Jolt 在 RegisterTypes 时会做 define 一致性自检并主动 abort，不会静默腐蚀堆。
    target_compile_definitions(Jolt PUBLIC JPH_PROFILE_ENABLED JPH_DEBUG_RENDERER)
endif()
# libEngine.a 会被链入 PrimalEngine.dylib（共享库），Jolt 静态目标必须 PIC
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Emscripten: Particles/*.cpp pulls in Metal headers and breaks the WASM build.
# Set the option BEFORE add_subdirectory(Engine) so the source-file filter runs.
if(EMSCRIPTEN)
    set(DISABLE_PARTICLE_SYSTEM ON CACHE BOOL "Force-disable particle system" FORCE)
endif()
add_subdirectory(Engine)
if(NOT EMSCRIPTEN)
    add_subdirectory(EngineDLL)
    add_subdirectory(EngineTest)
    # 离线通道 one-shot CLI（CLI 规划 §10 Phase 2）：engine info / render / schema
    add_subdirectory(Tools/PrimalEngineCli)
    # 运行时宿主（编辑器 Run 按钮启动目标，规划 §Run/Stop 2026-09-03）
    add_subdirectory(Tools/PrimalPlayer)
else()
    # Emscripten WASM test executable
    if(ENABLE_WEBGPU)
        # DISABLE_PARTICLE_SYSTEM was set above (before add_subdirectory(Engine))
        # so the source filter in Engine/CMakeLists.txt actually excludes
        # Particles/*.cpp. The test target still gets the compile definition
        # for safety in case any other translation unit reads it directly.
        add_executable(TestDawnWASM
            EngineTest/IntegrationTests/TestDawnMain.cpp
            EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
            EngineTest/IntegrationTests/EmscriptenInput.cpp
        )
        target_include_directories(TestDawnWASM PUBLIC
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/Engine
            ${CMAKE_SOURCE_DIR}/Engine/Common
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Platforms/Dawn
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph
            ${CMAKE_SOURCE_DIR}/third_party/stb
        )
        target_compile_definitions(TestDawnWASM PRIVATE
            ENABLE_WEBGPU=1
            TEST_DAWN_FORWARD_RENDERER=1
            DISABLE_PARTICLE_SYSTEM=1
            PRIMAL_PLUS
        )
        target_compile_options(TestDawnWASM PRIVATE "--use-port=emdawnwebgpu")
        target_link_libraries(TestDawnWASM PRIVATE Engine)
        target_link_options(TestDawnWASM PRIVATE
            "--use-port=emdawnwebgpu"
            -sALLOW_MEMORY_GROWTH=1
            -sMAXIMUM_MEMORY=2147418112
            -sINITIAL_MEMORY=268435456
            -sTOTAL_STACK=16777216
            -sENVIRONMENT=web
            -sEXIT_RUNTIME=0
            -sASYNCIFY
            -sASSERTIONS=1
            --shell-file ${CMAKE_SOURCE_DIR}/wasm/shell.html
            -g
            -O1
        )
        # Shaders are tiny (~50 KB total) so we embed them directly in the WASM
        # binary. This avoids the CMake @-escaping problem with --preload-file
        # and guarantees shaders are available in MEMFS before main() runs.
        set(EMBED_FLAGS "")
        set(SHADERS_ROOT "${CMAKE_SOURCE_DIR}/Engine/Graphics/Dawn/shaders")
        if(EXISTS "${SHADERS_ROOT}")
            string(APPEND EMBED_FLAGS " --embed-file ${SHADERS_ROOT}@/Engine/Graphics/Dawn/shaders")
        endif()
        # Embed Sponza.model (~7 MB) so it's in MEMFS before main() runs.
        # This avoids the async preRun timing issue where XHR fetch completes
        # after main() has already tried to open the file.
        set(SPONZA_MODEL "${CMAKE_SOURCE_DIR}/EngineTest/assets/Sponza.model")
        if(EXISTS "${SPONZA_MODEL}")
            string(APPEND EMBED_FLAGS " --embed-file ${SPONZA_MODEL}@/assets/Sponza.model")
        endif()
        # Embed DDGI static probe cache (~1.6 MB). Without this, WASM falls
        # back to StaticProbeBaker CPU ray trace at Mode 10/11 init — single-
        # threaded WASM makes that 30+ seconds, which looks like a tab hang.
        set(DDGI_CACHE "${CMAKE_SOURCE_DIR}/mode10_ddgi_cache.spch")
        if(EXISTS "${DDGI_CACHE}")
            string(APPEND EMBED_FLAGS " --embed-file ${DDGI_CACHE}@/mode10_ddgi_cache.spch")
        endif()
        # Export ccall/cwrap (used by shell.html to call setDebugParam/setSunDirection)
        # and export the two extern "C" functions themselves (EMSCRIPTEN_KEEPALIVE).
        # _main must be included because EXPORTED_FUNCTIONS replaces the default list.
        string(APPEND EMBED_FLAGS " -sEXPORTED_RUNTIME_METHODS=ccall,cwrap")
        string(APPEND EMBED_FLAGS " -sEXPORTED_FUNCTIONS=_main,_setDebugParam,_setSunDirection")
        if(NOT "${EMBED_FLAGS}" STREQUAL "")
            set_target_properties(TestDawnWASM PROPERTIES LINK_FLAGS "${EMBED_FLAGS}")
        endif()

        # Generate manifest for runtime-fetched assets (textures only — model is embedded)
        set(MANIFEST_FILE "${CMAKE_BINARY_DIR}/wasm_manifest.js")
        set(ASSETS_ROOT "${CMAKE_SOURCE_DIR}/EngineTest/assets")

        file(WRITE "${MANIFEST_FILE}" "// Auto-generated by CMake. Lists runtime-fetched assets.\nwindow.WASM_MANIFEST = {\n  shaders: [],\n  assets: [\n")
        if(EXISTS "${ASSETS_ROOT}/models/Sponza")
            file(GLOB_RECURSE SPONZA_TEX "${ASSETS_ROOT}/models/Sponza/*.png")
            foreach(f ${SPONZA_TEX})
                get_filename_component(name "${f}" NAME)
                file(APPEND "${MANIFEST_FILE}" "    { url: 'assets/models/Sponza/${name}', fsPath: '/assets/models/Sponza/${name}' },\n")
            endforeach()
        endif()
        file(APPEND "${MANIFEST_FILE}" "  ],\n};\n")

        add_custom_command(TARGET TestDawnWASM POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MANIFEST_FILE}" "${CMAKE_SOURCE_DIR}/wasm/wasm_manifest.js"
            COMMENT "Copying WASM asset manifest"
        )

        # ============================================================================
        # TestKenneyTilePreviewWASM — WFC showcase WASM binary.
        # Mirrors TestDawnWASM structure. Asset preload + POST_BUILD copy
        # added in Task 5.
        # ============================================================================
        add_executable(TestKenneyTilePreviewWASM
            EngineTest/IntegrationTests/TestKenneyMain.cpp
            EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.cpp
            EngineTest/IntegrationTests/Graphics/WFC/KenneyTileCatalog.cpp
            EngineTest/IntegrationTests/Graphics/WFC/Styles/KenneyDungeonStyle.cpp
            EngineTest/IntegrationTests/EmscriptenInput.cpp
        )
        target_include_directories(TestKenneyTilePreviewWASM PUBLIC
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/Engine
            ${CMAKE_SOURCE_DIR}/Engine/Common
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Platforms/Dawn
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph
            ${CMAKE_SOURCE_DIR}/third_party/stb
            ${CMAKE_SOURCE_DIR}/EngineTest
            ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests
            ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/Graphics/WFC/Styles
        )
        target_compile_definitions(TestKenneyTilePreviewWASM PRIVATE
            ENABLE_WEBGPU=1
            DISABLE_PARTICLE_SYSTEM=1
            PRIMAL_PLUS
        )
        target_compile_options(TestKenneyTilePreviewWASM PRIVATE "--use-port=emdawnwebgpu")
        target_link_libraries(TestKenneyTilePreviewWASM PRIVATE Engine)
        target_link_options(TestKenneyTilePreviewWASM PRIVATE
            "--use-port=emdawnwebgpu"
            -sALLOW_MEMORY_GROWTH=1
            -sMAXIMUM_MEMORY=2147418112
            -sINITIAL_MEMORY=268435456
            -sTOTAL_STACK=16777216
            -sENVIRONMENT=web
            -sEXIT_RUNTIME=0
            -sASYNCIFY
            -sASSERTIONS=1
            --shell-file ${CMAKE_SOURCE_DIR}/wasm/shell.html
            -g
            -O1
        )
        # Export ccall/cwrap + main, plus --embed-file flags for asset preload.
        # LINK_FLAGS is a single shell-evaluated string — combining exports and
        # embed-file in one property avoids CMake @-escaping issues and matches
        # the TestDawnWASM pattern.
        set(KENNEY_LINK_FLAGS "")
        string(APPEND KENNEY_LINK_FLAGS " -sEXPORTED_RUNTIME_METHODS=ccall,cwrap")
        string(APPEND KENNEY_LINK_FLAGS " -sEXPORTED_FUNCTIONS=_main,_wfc_set_observer,_wfc_set_origin,_wfc_set_grid_w,_wfc_set_grid_h,_wfc_set_grid_d,_wfc_reseed_same,_wfc_reseed_new,_wfc_get_observer,_wfc_get_origin,_wfc_get_grid_w,_wfc_get_grid_h,_wfc_get_grid_d,_wfc_get_style_count,_wfc_get_style_name,_wfc_get_style_display_name,_wfc_get_active_style_count,_wfc_get_active_style,_wfc_compose")

        # Embed WGSL shaders — same pattern as TestDawnWASM. The Dawn
        # ShaderLoader's MEMFS fallback (ShaderLoader.h:2276) reads from
        # /Engine/Graphics/Dawn/shaders/. Without this, every subsystem that
        # calls LoadWGSL silently no-ops and pipelines cascade-fail.
        set(SHADERS_ROOT "${CMAKE_SOURCE_DIR}/Engine/Graphics/Dawn/shaders")
        if(EXISTS "${SHADERS_ROOT}")
            string(APPEND KENNEY_LINK_FLAGS " --embed-file ${SHADERS_ROOT}@/Engine/Graphics/Dawn/shaders")
        endif()

        # Asset preload — embed Kenney tiles + colormap into the .wasm binary
        # (synchronous access from main(); --preload-file's async XHR would race).
        # Paths mirror what TestKenneyTilePreview.cpp's DirCandidate[] expects
        # (relative to CWD, which under WASM is "/"). Mount layout:
        #   /EngineTest/assets/Processed/kenney_dungeon_tiles/*.engine_mesh
        #   /EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png
        set(KENNEY_MESH_DIR "${CMAKE_SOURCE_DIR}/EngineTest/assets/Processed/kenney_dungeon_tiles")
        set(KENNEY_COLORMAP "${CMAKE_SOURCE_DIR}/EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png")
        if(EXISTS "${KENNEY_MESH_DIR}")
            string(APPEND KENNEY_LINK_FLAGS " --embed-file ${KENNEY_MESH_DIR}@/EngineTest/assets/Processed/kenney_dungeon_tiles")
        endif()
        if(EXISTS "${KENNEY_COLORMAP}")
            string(APPEND KENNEY_LINK_FLAGS " --embed-file ${KENNEY_COLORMAP}@/EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png")
        endif()
        set_target_properties(TestKenneyTilePreviewWASM PROPERTIES LINK_FLAGS "${KENNEY_LINK_FLAGS}")

        # ============================================================================
        # POST_BUILD copy — drop .js + .wasm into wfc/ so the launcher's
        # iframe finds them at the relative path wfc/shell.html expects.
        # (--embed-file packages assets into the .wasm, so there's no .data.)
        # ============================================================================
        set(WFC_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/wfc")
        file(MAKE_DIRECTORY "${WFC_DEPLOY_DIR}")
        add_custom_command(TARGET TestKenneyTilePreviewWASM POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:TestKenneyTilePreviewWASM>/TestKenneyTilePreviewWASM.js"
                "${WFC_DEPLOY_DIR}/TestKenneyTilePreviewWASM.js"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:TestKenneyTilePreviewWASM>/TestKenneyTilePreviewWASM.wasm"
                "${WFC_DEPLOY_DIR}/TestKenneyTilePreviewWASM.wasm"
            COMMENT "Copying TestKenneyTilePreviewWASM artifacts to wfc/"
        )
    endif()
endif()

# 设置构建顺序依赖
if(NOT EMSCRIPTEN)
    if(BUILD_CONTENT_TOOLS)
        add_dependencies(ContentTools Engine)
        add_dependencies(EngineDLL Engine ContentTools)
        if(APPLE)
            # EngineDLL hosts the editor render/present path. Keep its runtime Metal
            # shader blob in step with shader source changes so a normal native build
            # cannot leave the Avalonia app presenting through a stale metallib.
            add_dependencies(EngineDLL BuildEngineShaderBlob)
        endif()

        # 配置安装规则
        install(TARGETS Engine EngineDLL ContentTools
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
        )
    else()
        add_dependencies(EngineDLL Engine)
        if(APPLE)
            add_dependencies(EngineDLL BuildEngineShaderBlob)
        endif()
        message(STATUS "ContentTools disabled (BUILD_CONTENT_TOOLS=OFF)")

        install(TARGETS Engine EngineDLL
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
        )
    endif()
endif()

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
