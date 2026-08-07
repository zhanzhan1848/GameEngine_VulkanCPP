cmake_minimum_required(VERSION 3.15)
project(ContentTools)

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

# 添加源文件
set(PROJECTION_SOURCES
    ContentTools.cpp
    FbxImporter.cpp
    Geometry.cpp
    NormalMapIdentification.cpp
    ObjImporter.cpp
    PrimitiveMesh.cpp
    TextureImporter.cpp
    FbxImporter.h
    Geometry.h
    ObjImporter.h
    PrimitiveMesh.h
    NormalMapIdentification.h
    # Phase 1 asset pipeline shared sources (M2)
    common/SDF.h
    common/SDF.cpp
    common/ProcessableMesh.h
    common/ProcessableMesh.cpp
    common/ErrorReport.h
    common/PipelineTypes.h
    common/MeshConverters.h
    common/MeshConverters.cpp
)

set(MESHOPT_DIR "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src")

# 检查meshoptimizer是否存在
if(NOT EXISTS "${MESHOPT_DIR}/meshoptimizer.h")
    message(FATAL_ERROR "meshoptimizer submodule not found at ${MESHOPT_DIR}. Please run 'git submodule update --init --recursive'.")
endif()

# ---------------------------------------------------------------------------
# Phase 1 AI asset pipeline submodules (see Docs/2026-08-07-asset-pipeline-
# phase1-library-survey.md and ~/.claude/plans/memoized-dancing-boot.md).
# ---------------------------------------------------------------------------
set(PHASE1_SUBMODULES
    "third_party/eigen/Eigen/Dense"
    "third_party/libigl/include/igl/igl_inline.h"
    "third_party/pmp-library/CMakeLists.txt"
    "third_party/VHACD/src/VHACD_Lib/public/VHACD.h"
    "third_party/xatlas/source/xatlas/xatlas.h"
)
foreach(_submod_path IN LISTS PHASE1_SUBMODULES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_submod_path}")
        message(FATAL_ERROR "Phase 1 submodule missing: ${_submod_path}. "
            "Run 'git submodule update --init --recursive'.")
    endif()
endforeach()

set(MESHOPT_SOURCES
    ${MESHOPT_DIR}/meshoptimizer.h
    ${MESHOPT_DIR}/allocator.cpp
    ${MESHOPT_DIR}/clusterizer.cpp
    ${MESHOPT_DIR}/indexanalyzer.cpp
    ${MESHOPT_DIR}/indexcodec.cpp
    ${MESHOPT_DIR}/indexgenerator.cpp
    ${MESHOPT_DIR}/overdrawoptimizer.cpp
    ${MESHOPT_DIR}/partition.cpp
    ${MESHOPT_DIR}/quantization.cpp
    ${MESHOPT_DIR}/rasterizer.cpp
    ${MESHOPT_DIR}/simplifier.cpp
    ${MESHOPT_DIR}/spatialorder.cpp
    ${MESHOPT_DIR}/stripifier.cpp
    ${MESHOPT_DIR}/vcacheoptimizer.cpp
    ${MESHOPT_DIR}/vertexcodec.cpp
    ${MESHOPT_DIR}/vertexfilter.cpp
    ${MESHOPT_DIR}/vfetchoptimizer.cpp
    ${MESHOPT_DIR}/meshletutils.cpp
    ${MESHOPT_DIR}/meshletcodec.cpp
)

set(CONTENT_TOOLS_SOURCES
    ${PROJECTION_SOURCES}
    ${MESHOPT_SOURCES}
)

# 调试信息
message(STATUS "ContentTools Sources: ${CONTENT_TOOLS_SOURCES}")

# 添加头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${MESHOPT_DIR}
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC/
    ${MACOSX_PREFIX}/usr/include
    ${MACOSX_PREFIX}/System/Library/Frameworks
    # Phase 1 asset pipeline: header-only Eigen + libigl core (no [cgal]).
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/libigl/include
)

# Phase 1: PMP Library compiled static lib (repair / remesh / subdivide).
# Disable everything we don't need before add_subdirectory.
set(PMP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(PMP_BUILD_VIS OFF CACHE BOOL "" FORCE)
set(PMP_STRICT_COMPILATION OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/pmp-library ${CMAKE_BINARY_DIR}/pmp-library EXCLUDE_FROM_ALL)

# 创建库
add_library(${PROJECT_NAME} SHARED ${CONTENT_TOOLS_SOURCES})

# Phase 1 atomic modules. CMakeLists.txt are M1 stubs; M3-M7 populates
# each with module sources (Repair / Remesh / Subdivide / UVAtlas / Lod /
# Meshlet / Collision).
add_subdirectory(modules/repair)
add_subdirectory(modules/remesh)
add_subdirectory(modules/subdivide)
add_subdirectory(modules/uvatlas)
add_subdirectory(modules/lod)
add_subdirectory(modules/meshlet)
add_subdirectory(modules/collision)

# Phase 1 M8: pipeline orchestration layer (AssetPipeline + SceneBlobWriter).
add_subdirectory(pipeline)

# Phase 1 unit tests (M2.6+). Each module ships its own test binary here as
# it lands (M3 repair, M4 remesh, etc.).
add_subdirectory(tests)

# Compose Phase 1 module static libs into the SHARED ContentTools dylib.
# Each module exposes its symbols as PUBLIC so callers of the dylib can
# reach them.
target_link_libraries(${PROJECT_NAME} PRIVATE
    contenttools_repair
    contenttools_remesh
    contenttools_subdivide
    contenttools_uvatlas
    contenttools_lod
    contenttools_meshlet
    contenttools_collision
    contenttools_pipeline
)

# 平台特定设置
if(WIN32)
    # Windows平台设置
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WINDOWS
        _USRDLL
        _HAS_EXCEPTIONS=0
        UNICODE
        _UNICODE
    )
    
    # 查找FBX SDK
    find_package(FBX REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FBX_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FBX_LIBRARIES})
    
    # 查找DirectXTex
    find_package(DirectXTex REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE ${DirectXTex_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DirectXTex_LIBRARIES})

elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        APPLE
        __APPLE__
    )

    # 查找必要的框架
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(COCOA_LIBRARY Cocoa)

    # 动态查找 FBX SDK 库
    find_library(FBX_LIB_DEBUG
        NAMES fbxsdk
        HINTS ${FBX_SDK_ROOT}/lib
        PATH_SUFFIXES debug debug
        NO_DEFAULT_PATH
    )
    
    find_library(FBX_LIB_RELEASE
        NAMES fbxsdk
        HINTS ${FBX_SDK_ROOT}/lib
        PATH_SUFFIXES release release
        NO_DEFAULT_PATH
    )

    if (NOT FBX_LIB_DEBUG OR NOT FBX_LIB_RELEASE)
        message(FATAL_ERROR "Could not find FBX SDK libraries in ${FBX_SDK_ROOT}/lib. Checked debug/clang/debug and release/clang/release.")
    endif()

    # 链接 FBX SDK
    if(FBX_SDK_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE fbxsdk)
    endif()
    find_library(METAL_LIBRARY Metal)
    find_library(METALKIT_LIBRARY MetalKit)
    find_library(QUARTZ_CORE_LIBRARY QuartzCore)

    target_link_libraries(${PROJECT_NAME} 
        PRIVATE 
        debug ${FBX_LIB_DEBUG}
        optimized ${FBX_LIB_RELEASE}
        ${FOUNDATION_LIBRARY}
        ${COCOA_LIBRARY}
        ${METAL_LIBRARY}
        ${METALKIT_LIBRARY}
        ${QUARTZ_CORE_LIBRARY}

        "-framework CoreGraphics"
        "-framework ImageIO"
    )
    # 为 FBX SDK 禁用特定警告
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
        -Wno-deprecated-copy
        -Wno-unknown-pragmas
        -std=c++17
        -stdlib=libc++
    )

else()
    # Linux/MacOS平台设置
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        LINUX
    )
    
    # 查找OpenGL
    # find_package(OpenGL REQUIRED)
    # target_include_directories(${PROJECT_NAME} PRIVATE ${OPENGL_INCLUDE_DIRS})
    # target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES})
endif()

# 编译器选项
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
        /MP
        /permissive-
        /fp:fast
        /GR-
    >
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
        -Wall
        -Wextra
        -fPIC
        -Wno-uninitialized-const-reference
    >
)

# 链接选项
target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /DEBUG
        /INCREMENTAL
    >
)

# 安装目标
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# CLI Tool for converting FBX to intermediate binary format
if(APPLE OR UNIX)
    add_executable(ContentToolsCLI main.cpp)
    
    target_include_directories(ContentToolsCLI PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${MESHOPT_DIR}
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC/
    )

    target_link_libraries(ContentToolsCLI PRIVATE ${PROJECT_NAME})
    
    if(APPLE)
        target_compile_options(ContentToolsCLI PRIVATE -std=c++17 -stdlib=libc++)
    endif()
endif()