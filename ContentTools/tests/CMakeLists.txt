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

target_compile_options(TestMeshConverters PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
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

target_compile_options(TestRepair PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestRemesh PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestSubdivide PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestUvAtlas PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestLod PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestMeshlet PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestCollision PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestSceneBlobWriter PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestAssetPipeline PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
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

target_compile_options(TestSceneBlobReader PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
)

add_test(NAME TestSceneBlobReader COMMAND TestSceneBlobReader)
set_target_properties(TestSceneBlobReader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/ContentTools
)
