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
