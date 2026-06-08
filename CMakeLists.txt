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

# Emscripten WASM build support
if(EMSCRIPTEN)
    add_definitions(-D__EMSCRIPTEN__)
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
endif()

# 设置构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type: Debug Release" FORCE)
endif()

# 设置输出目录
if(EMSCRIPTEN)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/WASM)
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

    # 检查 submodule 是否已初始化
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src/meshoptimizer.h")
        message(FATAL_ERROR
            "Git submodules not initialized!\n"
            "Please run the following command to initialize submodules:\n"
            "    git submodule update --init --recursive"
        )
    endif()

    # === PhysX (Engine/Simulation/) ===
    # TODO: 使用 git submodule 集成 PhysX 源码：
    #   git submodule add https://github.com/NVIDIAGameWorks/PhysX.git third_party/PhysX
    # Engine/Simulation/ 中保留了 include 头文件和桥接层，预编译二进制已移除。
    # 当前状态：未集成到 CMake 构建系统，代码中暂无使用。
    message(STATUS "PhysX: not yet integrated as submodule (Engine/Simulation/ has headers only)")
    
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
if(NOT EMSCRIPTEN)
    include_directories(${CMAKE_SOURCE_DIR}/OSAPI/MAC)
endif()

# 添加子项目
if(NOT EMSCRIPTEN)
    add_subdirectory(ContentTools)
endif()
# Emscripten: Particles/*.cpp pulls in Metal headers and breaks the WASM build.
# Set the option BEFORE add_subdirectory(Engine) so the source-file filter runs.
if(EMSCRIPTEN)
    set(DISABLE_PARTICLE_SYSTEM ON CACHE BOOL "Force-disable particle system" FORCE)
endif()
add_subdirectory(Engine)
if(NOT EMSCRIPTEN)
    add_subdirectory(EngineDLL)
    add_subdirectory(EngineTest)
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
    endif()
endif()

# 设置构建顺序依赖
if(NOT EMSCRIPTEN)
    add_dependencies(ContentTools Engine)
    add_dependencies(EngineDLL Engine ContentTools)

    # 配置安装规则
    install(TARGETS Engine EngineDLL ContentTools
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
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
