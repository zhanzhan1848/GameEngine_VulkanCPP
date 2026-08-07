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
