cmake_minimum_required(VERSION 3.20)

project(IntegrationTests)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_HEADERS
    "PythonScript.h"
    "ShaderCompilation.h"
    "Test.h"
    "TestEntityComponents.h"
    "TestRenderer.h"
    "TestWindow.h"
    "MacKeyboard.h"
)

# 辅助函数：配置测试目标
function(setup_test_target target_name)
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
    )
    target_include_directories(${target_name} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Common"
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC/
    )
    
    # 定义测试宏
    target_compile_definitions(${target_name} PRIVATE "PRIMAL_PLUS")
    
    if(APPLE)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS")
        target_compile_definitions(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:DEBUG>"
            "$<$<CONFIG:Release>:NDEBUG>"
        )
        target_compile_options(${target_name} PRIVATE
            -Wall
            -ffast-math
            -pthread
            $<$<CONFIG:Debug>:-O0 -g>
            $<$<CONFIG:Release>:-O3>
            -Wunused-parameter
        )
        target_link_libraries(${target_name} PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework AppKit"
        )
    elseif(WIN32)
        target_compile_definitions(${target_name} PRIVATE
            "_CONSOLE"
            "_HAS_EXCEPTIONS=0"
            "UNICODE"
            "_UNICODE"
            "$<$<CONFIG:Debug>:DEBUG>"
            "$<$<CONFIG:Release>:NDEBUG>"
        )
        target_compile_options(${target_name} PRIVATE
            /W3 /MP /GR- /fp:fast /EHs-c-
            $<$<CONFIG:Debug>:/Od /Zi>
            $<$<CONFIG:Release>:/O2>
        )
        target_link_options(${target_name} PRIVATE
            /SUBSYSTEM:WINDOWS
            /IGNORE:4099
            $<$<CONFIG:Debug>:/DEBUG>
            $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
        )
    elseif(LINUX)
        target_compile_definitions(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:DEBUG>"
            "$<$<CONFIG:Release>:NDEBUG>"
        )
        target_compile_options(${target_name} PRIVATE
            -Wall -ffast-math -fno-exceptions
            $<$<CONFIG:Debug>:-O0 -g>
            $<$<CONFIG:Release>:-O3>
        )
        target_link_libraries(${target_name} PRIVATE X11 vulkan)
    endif()

    target_link_libraries(${target_name} PRIVATE Engine ContentTools)
    
    # 复制着色器和资源文件
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
        $<TARGET_FILE_DIR:${target_name}>/shaders
    )
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets
        $<TARGET_FILE_DIR:${target_name}>/assets
    )
    
    # 注册测试 (使用 Engine_Integration_Tests_ 前缀以便被 run_integration_tests 目标捕获)
    add_test(NAME Engine_Integration_Tests_${target_name} COMMAND ${target_name})
endfunction()

enable_testing()

if(APPLE)
    # 1. TestRendererMac
    add_executable(TestRendererMac
        "Main.cpp"
        "TestRendererMac.cpp"
        "Lights.cpp"
        "RenderItem.cpp"
        "Scripts.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRendererMac)

    # 2. TestMetalRenderer
    add_executable(TestMetalRenderer
        "Main.cpp"
        "TestMetalRenderer.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMetalRenderer)

    # 3. TestCSMIntegration
    add_executable(TestCSMIntegration
        "Main.cpp"
        "TestCSMIntegration.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestCSMIntegration)

    # 4. TestStandardPipeline
    add_executable(TestStandardPipeline
        "Main.cpp"
        "TestStandardPipeline.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestStandardPipeline)
    target_compile_definitions(TestStandardPipeline PRIVATE "TEST_STANDARD_PIPELINE=1" "TEST_RENDERER=0")

    # 5. TestCSMIntegrationRenderGraph
    add_executable(TestCSMIntegrationRenderGraph
        "Main.cpp"
        "TestCSMIntegrationRenderGraph.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestCSMIntegrationRenderGraph)
    target_compile_definitions(TestCSMIntegrationRenderGraph PRIVATE "TEST_CSM_RENDERGRAPH=1" "TEST_RENDERER=0")

    # 6. TestMultiView
    add_executable(TestMultiView
        "Main.cpp"
        "TestMultiView.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestMultiView)
    target_compile_definitions(TestMultiView PRIVATE "TEST_MULTIVIEW=1" "TEST_RENDERER=0")

    # 7. TestSponzaRenderGraph
    add_executable(TestSponzaRenderGraph
        "Main.cpp"
        "TestSponzaRenderGraph.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestSponzaRenderGraph)
    target_compile_definitions(TestSponzaRenderGraph PRIVATE "TEST_SPONZA_RENDERGRAPH=1" "TEST_RENDERER=0")

    # 8. TestGeometryDebugSponza
    add_executable(TestGeometryDebugSponza
        "Main.cpp"
        "TestGeometryDebugSponza.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestGeometryDebugSponza)
    target_compile_definitions(TestGeometryDebugSponza PRIVATE "TEST_GEOMETRY_DEBUG_SPONZA=1" "TEST_RENDERER=0")

    # 9. TestParticleSponza
    add_executable(TestParticleSponza
        "Main.cpp"
        "TestParticleSponza.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestParticleSponza)
    target_compile_definitions(TestParticleSponza PRIVATE "TEST_PARTICLE_SPONZA=1" "TEST_RENDERER=0")

    # 10. TestNaniteStreamingPipeline
    add_executable(TestNaniteStreamingPipeline
        "Main.cpp"
        "TestNaniteStreamingPipeline.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestNaniteStreamingPipeline)
    target_compile_definitions(TestNaniteStreamingPipeline PRIVATE "TEST_NANITE_STREAMING_PIPELINE=1" "TEST_RENDERER=0")


    # Create IntegrationTests pseudo-target
    add_custom_target(IntegrationTests DEPENDS TestRendererMac TestMetalRenderer TestCSMIntegration TestStandardPipeline TestCSMIntegrationRenderGraph TestMultiView TestSponzaRenderGraph TestGeometryDebugSponza TestParticleSponza TestNaniteStreamingPipeline)

elseif(WIN32)
    # Windows下使用 TestRenderer
    add_executable(TestRenderer
        "Lights.cpp"
        "Main.cpp"
        "RenderItem.cpp"
        "Scripts.cpp"
        "ShaderCompilation.cpp"
        "TestRenderer.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRenderer)
    
    add_custom_target(IntegrationTests DEPENDS TestRenderer)

elseif(LINUX)
    # Linux下使用 TestRenderer (假设兼容)
    add_executable(TestRenderer
        "Lights.cpp"
        "Main.cpp"
        "RenderItem.cpp"
        "Scripts.cpp"
        "ShaderCompilation.cpp"
        "TestRenderer.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRenderer)
    
    add_custom_target(IntegrationTests DEPENDS TestRenderer)
endif()
