# Phase 1 ContentTools unit tests.
#
# TestMeshConverters exercises the M2 IR round-trip:
#   legacy mesh → ProcessableMesh IR → legacy mesh
# plus validate_invariants accept/reject coverage.
#
# Standalone test binary — links only against ContentTools.dylib for the
# symbols in common/*.cpp. No Engine runtime dependency beyond headers
# pulled in transitively via ToolsCommon.h / Geometry.h.

add_executable(TestMeshConverters TestMeshConverters.cpp)

target_include_directories(TestMeshConverters PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestMeshConverters PRIVATE ContentTools)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestMeshConverters PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    >
)

add_test(NAME TestMeshConverters COMMAND TestMeshConverters)
set_target_properties(TestMeshConverters PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M3 repair module test ------------------------------------------------

add_executable(TestRepair TestRepair.cpp)

target_include_directories(TestRepair PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/pmp-library/src
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestRepair PRIVATE ContentTools contenttools_repair)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestRepair PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestRepair COMMAND TestRepair)
set_target_properties(TestRepair PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M4 remesh module test ------------------------------------------------

add_executable(TestRemesh TestRemesh.cpp)

target_include_directories(TestRemesh PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestRemesh PRIVATE ContentTools contenttools_remesh)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestRemesh PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestRemesh COMMAND TestRemesh)
set_target_properties(TestRemesh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M5 subdivide module test ---------------------------------------------

add_executable(TestSubdivide TestSubdivide.cpp)

target_include_directories(TestSubdivide PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestSubdivide PRIVATE ContentTools contenttools_subdivide)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestSubdivide PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestSubdivide COMMAND TestSubdivide)
set_target_properties(TestSubdivide PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M6 uvatlas module test -----------------------------------------------

add_executable(TestUvAtlas TestUvAtlas.cpp)

target_include_directories(TestUvAtlas PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestUvAtlas PRIVATE ContentTools contenttools_uvatlas)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestUvAtlas PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestUvAtlas COMMAND TestUvAtlas)
set_target_properties(TestUvAtlas PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M7 lod module test ---------------------------------------------------

add_executable(TestLod TestLod.cpp)

target_include_directories(TestLod PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestLod PRIVATE ContentTools contenttools_lod)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestLod PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestLod COMMAND TestLod)
set_target_properties(TestLod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M7 meshlet module test -----------------------------------------------

add_executable(TestMeshlet TestMeshlet.cpp)

target_include_directories(TestMeshlet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestMeshlet PRIVATE ContentTools contenttools_meshlet)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestMeshlet PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestMeshlet COMMAND TestMeshlet)
set_target_properties(TestMeshlet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M7 collision module test ---------------------------------------------

add_executable(TestCollision TestCollision.cpp)

target_include_directories(TestCollision PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestCollision PRIVATE ContentTools contenttools_collision)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestCollision PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestCollision COMMAND TestCollision)
set_target_properties(TestCollision PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M8 SceneBlobWriter test ----------------------------------------------

add_executable(TestSceneBlobWriter TestSceneBlobWriter.cpp)

target_include_directories(TestSceneBlobWriter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestSceneBlobWriter PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestSceneBlobWriter PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestSceneBlobWriter COMMAND TestSceneBlobWriter)
set_target_properties(TestSceneBlobWriter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M8 AssetPipeline integration test ------------------------------------

add_executable(TestAssetPipeline TestAssetPipeline.cpp)

target_include_directories(TestAssetPipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestAssetPipeline PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestAssetPipeline PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestAssetPipeline COMMAND TestAssetPipeline)
set_target_properties(TestAssetPipeline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M9 SceneBlobReader test ----------------------------------------------
# Verifies the reader correctly skips the pack_data scene wrapper
# (scene_name + materials + lod_group wrapper) before walking meshes.

add_executable(TestSceneBlobReader TestSceneBlobReader.cpp)

target_include_directories(TestSceneBlobReader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestSceneBlobReader PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestSceneBlobReader PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestSceneBlobReader COMMAND TestSceneBlobReader)
set_target_properties(TestSceneBlobReader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M11 derive_attributes module test ------------------------------------

add_executable(TestDeriveAttributes TestDeriveAttributes.cpp)

target_include_directories(TestDeriveAttributes PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestDeriveAttributes PRIVATE ContentTools contenttools_derive)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestDeriveAttributes PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestDeriveAttributes COMMAND TestDeriveAttributes)
set_target_properties(TestDeriveAttributes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- M12 PackedMeshDecoder + ReadSceneHeader test --------------------------
# Round-trip tests for all 4 static_* elements_types + edge cases (index
# widening, empty mesh, multi-mesh scene, truncated buffer).

add_executable(TestPackedMeshDecoder TestPackedMeshDecoder.cpp)

target_include_directories(TestPackedMeshDecoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestPackedMeshDecoder PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestPackedMeshDecoder PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    >
)

add_test(NAME TestPackedMeshDecoder COMMAND TestPackedMeshDecoder)
set_target_properties(TestPackedMeshDecoder PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- T4.2 card bake test ----------------------------------------------------
# Synthetic thin-plate mesh → 6-direction bake: thin-view culling, tight
# projections, surfel generation, open-geometry fallback, blob round trip.

add_executable(TestCardBake TestCardBake.cpp)

target_include_directories(TestCardBake PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

# R13-C/T2（2026-11-04 Windows 十一测 LNK2019）：GetSceneSize/SerializeScene
# 定义于 SceneBlobWriter.cpp、WalkSceneBlob 定义于 SceneBlobReader.cpp，两者
# 只编入 contenttools_pipeline 静态库。macOS 上 ContentTools 为 SHARED dylib
# 且默认导出 PRIVATE 链入的静态库符号（nm -gU 实证三符号导出），故仅链
# ContentTools 也能解析；Windows DLL 只导出 __declspec(dllexport) 符号，
# pipeline 全体无 dllexport，导入库不含三符号 → LNK2019。补链静态库与
# TestSceneBlobWriter/TestAssetPipeline/TestPackedMeshDecoder/TestGltfImporter
# 同模式（对齐本文件既有惯例）。
target_link_libraries(TestCardBake PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestCardBake PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    >
)

add_test(NAME TestCardBake COMMAND TestCardBake)
set_target_properties(TestCardBake PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# ---- glTF 2.0 importer test (.glb container / .gltf JSON / Draco reject) ----

add_executable(TestGltfImporter TestGltfImporter.cpp)

target_include_directories(TestGltfImporter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)

target_link_libraries(TestGltfImporter PRIVATE ContentTools contenttools_pipeline)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(TestGltfImporter PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    >
)

add_test(NAME TestGltfImporter COMMAND TestGltfImporter)
set_target_properties(TestGltfImporter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)

# Legacy corner tangent generation, shared with Geometry.cpp.
add_executable(TestLegacyTangents TestLegacyTangents.cpp)
target_link_libraries(TestLegacyTangents PRIVATE contenttools_derive)
add_test(NAME TestLegacyTangents COMMAND TestLegacyTangents)
set_target_properties(TestLegacyTangents PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools)

add_executable(TestLegacyTangentPipeline TestLegacyTangentPipeline.cpp)
target_include_directories(TestLegacyTangentPipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common ${CMAKE_SOURCE_DIR}/OSAPI/MAC)
target_link_libraries(TestLegacyTangentPipeline PRIVATE ContentTools)
add_test(NAME TestLegacyTangentPipeline COMMAND TestLegacyTangentPipeline)
set_target_properties(TestLegacyTangentPipeline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools)
if(APPLE AND FBX_LIB_DEBUG)
    # ContentTools links the SDK with @executable_path; this test executable
    # lives outside the CLI's directory containing the SDK runtime copy.
    get_filename_component(LEGACY_TANGENT_FBX_DIR "${FBX_LIB_DEBUG}" DIRECTORY)
    set_tests_properties(TestLegacyTangentPipeline PROPERTIES
        ENVIRONMENT "DYLD_LIBRARY_PATH=${LEGACY_TANGENT_FBX_DIR}:$<TARGET_FILE_DIR:ContentTools>")
endif()
