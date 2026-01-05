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
)

set(MESHOPT_DIR "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src")
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
)

set(SOURCES
    ${PROJECTION_SOURCES}
    ${MESHOPT_SOURCES}
)

# 添加头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${MESHOPT_DIR}
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC/
    ${MACOSX_PREFIX}/usr/include
    ${MACOSX_PREFIX}/System/Library/Frameworks
)

# 创建库
add_library(${PROJECT_NAME} SHARED ${SOURCES})

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

    # 链接 FBX SDK
    if(FBX_SDK_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE fbxsdk)
    endif()
    find_library(METAL_LIBRARY Metal)
    find_library(METALKIT_LIBRARY MetalKit)
    find_library(QUARTZ_CORE_LIBRARY QuartzCore)

    target_link_libraries(${PROJECT_NAME} 
        PRIVATE 
        debug ${FBX_SDK_ROOT}/lib/clang/debug/libfbxsdk.dylib
        optimized ${FBX_SDK_ROOT}/lib/clang/release/libfbxsdk.dylib
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
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
