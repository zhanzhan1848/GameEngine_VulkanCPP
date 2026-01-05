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

    # FBX SDK 自动检测和安装
    set(FBX_SDK_VERSION "2020.3.7")
    set(FBX_SDK_ROOT "/Applications/Autodesk/FBX SDK/${FBX_SDK_VERSION}")
    set(FBX_SDK_FOUND FALSE)
    
    # 检查多个可能的FBX SDK安装路径
    set(FBX_SDK_POSSIBLE_PATHS
        "${FBX_SDK_ROOT}"
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
            set(FBX_SDK_ROOT ${PATH})
            set(FBX_SDK_FOUND TRUE)
            break()
        endif()
    endforeach()
    
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
        if(EXISTS "${FBX_SDK_ROOT}/lib/clang/debug")
            link_directories("${FBX_SDK_ROOT}/lib/clang/debug")
        endif()
        if(EXISTS "${FBX_SDK_ROOT}/lib/clang/release")
            link_directories("${FBX_SDK_ROOT}/lib/clang/release")
        endif()
        
        # 复制FBX SDK动态库到输出目录
        if(EXISTS "${FBX_SDK_ROOT}/lib/clang/debug/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/clang/debug/libfbxsdk.dylib" 
                 DESTINATION ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        elseif(EXISTS "${FBX_SDK_ROOT}/lib/clang/release/libfbxsdk.dylib")
            file(COPY "${FBX_SDK_ROOT}/lib/clang/release/libfbxsdk.dylib" 
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

# 配置安装规则
install(TARGETS Engine EngineDLL ContentTools
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
