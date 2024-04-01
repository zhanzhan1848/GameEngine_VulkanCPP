set(PROJECT_NAME EngineDLL)

################################################################################
# Source groups
################################################################################
set(
    "Common.h"
)
source_group("头文件" FILES ${})

set(
    "DllMain.cpp"
    "EngineAPI.cpp"
    "EntityAPI.cpp"
)
source_group("源文件" FILES ${})

set(ALL_FILES
    ${}
    ${}
)

################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} SHARED ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE EngineDLL)

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_GLOBAL_KEYWORD "Win32Proj"
)
################################################################################
# Target name
################################################################################
set_target_properties(${PROJECT_NAME} PROPERTIES
    TARGET_NAME_DEBUG         "${PROJECT_NAME}"
    TARGET_NAME_DEBUGEDITOR   "${PROJECT_NAME}"
    TARGET_NAME_RELEASE       "${PROJECT_NAME}"
    TARGET_NAME_RELEASEEDITOR "${PROJECT_NAME}"
)
################################################################################
# Output directory
################################################################################
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_DIRECTORY_DEBUG         "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
    OUTPUT_DIRECTORY_DEBUGEDITOR   "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
    OUTPUT_DIRECTORY_RELEASE       "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
    OUTPUT_DIRECTORY_RELEASEEDITOR "${CMAKE_SOURCE_DIR}/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG         "${CMAKE_CURRENT_SOURCE_DIR}/"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUGEDITOR   "${CMAKE_CURRENT_SOURCE_DIR}/"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE       "${CMAKE_CURRENT_SOURCE_DIR}/"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASEEDITOR "${CMAKE_CURRENT_SOURCE_DIR}/"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    ARCHIVE_OUTPUT_NAME_DEBUG         "${PROJECT_NAME}"
    ARCHIVE_OUTPUT_NAME_DEBUGEDITOR   "${PROJECT_NAME}"
    ARCHIVE_OUTPUT_NAME_RELEASE       "${PROJECT_NAME}"
    ARCHIVE_OUTPUT_NAME_RELEASEEDITOR "${PROJECT_NAME}"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASEEDITOR "TRUE"
)
################################################################################
# MSVC runtime library
################################################################################
get_property(MSVC_RUNTIME_LIBRARY_DEFAULT TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY)
string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
    $<$<CONFIG:Debug>:
        MultiThreadedDebugDLL
    >
    $<$<CONFIG:DebugEditor>:
        MultiThreadedDebugDLL
    >
    $<$<CONFIG:Release>:
        MultiThreadedDebugDLL
    >
    $<$<CONFIG:ReleaseEditor>:
        MultiThreadedDLL
    >
    $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugEditor>,$<CONFIG:Release>,$<CONFIG:ReleaseEditor>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
)
set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})

################################################################################
# Include directories
################################################################################
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../Engine;"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Engine/Common"
)

################################################################################
# Compile definitions
################################################################################
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Debug>:"
        "DEBUG"
    ">"
    "$<$<CONFIG:DebugEditor>:"
        "DEBUG"
    ">"
    "$<$<CONFIG:Release>:"
        "DEBUG"
    ">"
    "$<$<CONFIG:ReleaseEditor>:"
        "NDEBUG"
    ">"
    "PRIMAL_PLUS;"
    "USE_WITH_EDITOR;"
    "ENGINEDLL_EXPORTS;"
    "_WINDOWS;"
    "_USRDLL;"
    "_HAS_EXCEPTIONS=0;"
    "UNICODE;"
    "_UNICODE"
)

################################################################################
# Compile and link options
################################################################################
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /Od;
            /ZI
        >
        $<$<CONFIG:DebugEditor>:
            /Od;
            /ZI
        >
        $<$<CONFIG:Release>:
            /Od;
            /ZI
        >
        $<$<CONFIG:ReleaseEditor>:
            /Ox;
            /Oi;
            /GF;
            /Oy;
            /Gy;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            /GS-
        >
        /permissive-;
        /MP;
        /std:c++17;
        /Gm-;
        /W4;
        /fp:fast;
        /GR-;
        /Y-
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
            /DEBUG;
            /INCREMENTAL
        >
        $<$<CONFIG:DebugEditor>:
            /DEBUG;
            /INCREMENTAL
        >
        $<$<CONFIG:Release>:
            /DEBUG;
            /INCREMENTAL
        >
        $<$<CONFIG:ReleaseEditor>:
            /DEBUG:FULL;
            /OPT:REF;
            /OPT:ICF;
            /INCREMENTAL:NO
        >
        /SUBSYSTEM:WINDOWS;
        /ignore:4099
    )
endif()

################################################################################
# Dependencies
################################################################################
add_dependencies(${PROJECT_NAME}
    ContentTools
    Engine
)

target_link_directories(${PROJECT_NAME} PUBLIC
    "${OUTPUT_DIRECTORY}"
)

