# Phase 1 meshlet module (meshoptimizer).
#
# Meshlet.{h,cpp} wraps meshopt_buildMeshlets with the runtime contract
# 256-vertex / 128-triangle cap (Engine/Graphics/Nanite/MeshletSynthesis.h).
# Geometry.cpp:291-301's legacy 64/124 path is preserved untouched.
#
# meshopt is compiled into the ContentTools dylib via the parent CMake.
# This static lib links against the dylib at the parent level (symbols
# resolved at link time).

set(_meshlet_sources
    Meshlet.h
    Meshlet.cpp
)

add_library(contenttools_meshlet STATIC ${_meshlet_sources})

target_include_directories(contenttools_meshlet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src
)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(contenttools_meshlet PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
    >
)

# R4（Windows 四测）：Meshlet.cpp 在 MSVC 下触发 C1001 ICE（xutility std::copy
# 实例化点，用户日志 Meshlet.cpp(113)）。已做根治性微重构（聚合临时
# emplace_back → 命名局部 + push_back(std::move)），本条 /Od 是双保险兜底
# ——对齐 Engine/CMakeLists.txt LumenDDGIPass.cpp 先例（R2-5/R3-5）：per-source
# 选项追加在 target 选项之后（非 Debug 的 /Ox 被压住；Debug 本就 /Od）；
# 属性读回 + STATUS 凭证（Windows configure 输出必须含下述行，空=未生效）。
if(MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Meshlet.cpp"
        PROPERTIES COMPILE_OPTIONS "/Od")
    get_source_file_property(PRIMAL_MESHLET_ICE_OD_FLAGS
        "${CMAKE_CURRENT_SOURCE_DIR}/Meshlet.cpp" COMPILE_OPTIONS)
    message(STATUS "C1001 mitigation (meshlet): Meshlet.cpp='${PRIMAL_MESHLET_ICE_OD_FLAGS}'")
endif()
