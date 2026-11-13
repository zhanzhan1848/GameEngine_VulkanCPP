# vtbake — VT M1 离线烘焙 CLI(跨平台独立目标,无 FBX/DirectXTex 依赖)
# 依赖:stb(头文件)、astcenc-native-static(根 CMake 配置)、zlib
cmake_minimum_required(VERSION 3.20)
project(vtbake CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vtbake
    main.cpp
    ${CMAKE_SOURCE_DIR}/ContentTools/common/VTBaker.cpp
)

target_include_directories(vtbake PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/ContentTools/common
    ${CMAKE_SOURCE_DIR}/third_party/astc-encoder/Source
)

# R4（Windows 四测）：Windows 无系统 zlib——用根 CMakeLists WIN32 段 FetchContent
# 拉取的 zlibstatic（ALIAS primal_zlib_static，定义在本 add_subdirectory 之前）；
# 其余平台原样。
if(WIN32 AND TARGET primal_zlib_static)
    target_link_libraries(vtbake PRIVATE astcenc-native-static primal_zlib_static)
else()
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(vtbake PRIVATE astcenc-native-static ZLIB::ZLIB)
    else()
        target_link_libraries(vtbake PRIVATE astcenc-native-static z)
    endif()
endif()

if(NOT WIN32)
    set_target_properties(vtbake PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tools
    )
endif()
