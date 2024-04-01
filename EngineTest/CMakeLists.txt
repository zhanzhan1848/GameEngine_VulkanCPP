set(PROJECT_NAME EngineTest)

################################################################################
# Source groups
################################################################################
set(no_group_source_files
    "Lights.cpp"
    "Main.cpp"
    "PythonScript.h"
    "RenderItem.cpp"
    "Scripts.cpp"
    "ShaderCompilation.cpp"
    "ShaderCompilation.h"
    "Test.h"
    "TestEntityComponents.h"
    "TestRenderer.cpp"
    "TestRenderer.h"
    "TestWindow.h"
    "TestWindowLinux.h"
)
source_group("" FILES ${no_group_source_files})

set(ALL_FILES
    ${no_group_source_files}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE EngineTest)

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
    INTERPROCEDURAL_OPTIMIZATION_RELEASE       "TRUE"
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
        MultiThreadedDLL
    >
    $<$<CONFIG:ReleaseEditor>:
        MultiThreadedDebugDLL
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
        "NDEBUG"
    ">"
    "$<$<CONFIG:ReleaseEditor>:"
        "DEBUG"
    ">"
    "PRIMAL_PLUS;"
    "_CONSOLE;"
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
            /sdl;
            /W3;
            /ZI
        >
        $<$<CONFIG:DebugEditor>:
            /Od;
            /sdl;
            /W3;
            /ZI
        >
        $<$<CONFIG:Release>:
            /Ox;
            /Oi;
            /GF;
            /Oy;
            /Gy;
            /W4;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            /GS-
        >
        $<$<CONFIG:ReleaseEditor>:
            /Od;
            /sdl;
            /W3;
            /ZI
        >
        /permissive-;
        /MP;
        /std:c++17;
        /Gm-;
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
            /DEBUG:FULL;
            /OPT:REF;
            /OPT:ICF;
            /INCREMENTAL:NO
        >
        $<$<CONFIG:ReleaseEditor>:
            /DEBUG;
            /INCREMENTAL
        >
        /SUBSYSTEM:WINDOWS;
        /ignore:4099
    )
endif()

################################################################################
# Pre build events
################################################################################
add_custom_command_if(
    TARGET ${PROJECT_NAME}
    PRE_BUILD
    COMMANDS
    COMMAND         $<CONFIG:Debug> powershell -ExecutionPolicy Bypass -File $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>GetDXC.ps1 $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler
    COMMAND         $<CONFIG:Debug> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxcompiler.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND         $<CONFIG:Debug> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxil.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND   $<CONFIG:DebugEditor> powershell -ExecutionPolicy Bypass -File $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>GetDXC.ps1 $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler
    COMMAND   $<CONFIG:DebugEditor> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxcompiler.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND   $<CONFIG:DebugEditor> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxil.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND       $<CONFIG:Release> powershell -ExecutionPolicy Bypass -File $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>GetDXC.ps1 $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler
    COMMAND       $<CONFIG:Release> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxcompiler.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND       $<CONFIG:Release> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxil.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND $<CONFIG:ReleaseEditor> powershell -ExecutionPolicy Bypass -File $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>GetDXC.ps1 $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler
    COMMAND $<CONFIG:ReleaseEditor> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxcompiler.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMAND $<CONFIG:ReleaseEditor> xcopy /Y /D $<SHELL_PATH:${CMAKE_SOURCE_DIR}/>packages\\DirectXShaderCompiler\\bin\\x64\\dxil.dll $<SHELL_PATH:${OUTPUT_DIRECTORY}>
    COMMENT "If packages\DirectXShaderCompiler\ folder doesn't exist or is empty then download the latest release of DXC"
)

################################################################################
# Dependencies
################################################################################
add_dependencies(${PROJECT_NAME}
    ContentTools
    Engine
    EngineDLL
)

target_link_directories(${PROJECT_NAME} PRIVATE
    "${OUTPUT_DIRECTORY}"
)

use_package(${PROJECT_NAME} Microsoft.Direct3D.D3D12 1.608.0)
