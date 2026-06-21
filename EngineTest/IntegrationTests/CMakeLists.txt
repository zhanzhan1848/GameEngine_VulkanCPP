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
        ${CMAKE_SOURCE_DIR}/third_party/stb
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
    target_compile_definitions(TestRendererMac PRIVATE "TEST_RENDERER=1")

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

    # 5. TestModularPipeline
    add_executable(TestModularPipeline
        "Main.cpp"
        "TestModularPipeline.cpp"
        "ShaderCompilation.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/ShadowMapModule.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/DeferredLightingModule.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/FinalBlitModule.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/GIGatherModule.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/FusionCompositeModule.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderPipeline/Modules/SCDDGIIntegrationModule.cpp"
    )
    setup_test_target(TestModularPipeline)
    target_compile_definitions(TestModularPipeline PRIVATE "TEST_MODULAR_PIPELINE=1" "TEST_RENDERER=0")

    # 6. TestCSMIntegrationRenderGraph
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

    # 11. TestHZBStandalone
    add_executable(TestHZBStandalone
        "HZBStandaloneTest.cpp"
        "HZBTestUtils.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestHZBStandalone)
    target_compile_definitions(TestHZBStandalone PRIVATE "HZB_STANDALONE_TEST=1")

    # 12. TestForwardRenderer
    add_executable(TestForwardRenderer
        "Main.cpp"
        "TestForwardRenderer.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestForwardRenderer)
    target_compile_definitions(TestForwardRenderer PRIVATE "TEST_FORWARD_RENDERER=1" "TEST_RENDERER=0")

    # 13. TestPCGScatter
    add_executable(TestPCGScatter
        "Main.cpp"
        "TestPCGScatter.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestPCGScatter)
    target_compile_definitions(TestPCGScatter PRIVATE "TEST_PCG_SCATTER=1" "TEST_RENDERER=0")

    # 14. TestGeometryAPI (headless, no GPU)
    add_executable(TestGeometryAPI
        "Main.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestGeometryAPI)
    target_compile_definitions(TestGeometryAPI PRIVATE "TEST_GEOMETRY_API=1" "TEST_RENDERER=0")

    # 15. TestFieldDrivenScatter (headless, no GPU)
    add_executable(TestFieldDrivenScatter
        "Main.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestFieldDrivenScatter)
    target_compile_definitions(TestFieldDrivenScatter PRIVATE "TEST_FIELD_DRIVEN_SCATTER=1" "TEST_RENDERER=0")

    # 16. TestMaterialPreview (live material preview window)
    add_executable(TestMaterialPreview
        "Main.cpp"
        "TestMaterialPreview.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMaterialPreview)
    target_compile_definitions(TestMaterialPreview PRIVATE "TEST_MATERIAL_PREVIEW=1" "TEST_RENDERER=0")

    # 17. TestMaterialPreviewEditor (MaterialPreview C API validation)
    add_executable(TestMaterialPreviewEditor
        "Main.cpp"
        "TestMaterialPreviewEditor.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMaterialPreviewEditor)
    target_compile_definitions(TestMaterialPreviewEditor PRIVATE "TEST_MATERIAL_PREVIEW_EDITOR=1" "TEST_RENDERER=0")
    target_link_libraries(TestMaterialPreviewEditor PRIVATE EngineDLL)

    # 18. TestRendererRHIDevice (Phase 1 Sub-step 1.2.2)
    # 验证 graphics::initialize_with_device / shutdown_rhi / is_rhi_initialized / get_rhi_device
    # 通过 Renderer 公开 API 拿到 device，再 CreateBuffer 验证 device 真正可用。
    add_executable(TestRendererRHIDevice
        "TestRendererRHIDevice.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRendererRHIDevice)
    target_link_libraries(TestRendererRHIDevice PRIVATE Engine)

    # 19. TestEngineDLLLifecycle (Phase 1 Sub-step 1.2.5')
    # 通过 dlopen + dlsym 验证 EngineDLL 暴露的 C ABI：InitializeEngine / ShutdownEngine /
    # IsEngineInitialized / GetEngineDeviceHandle。模拟 Editor (C#) P/Invoke 的调用路径。
    add_executable(TestEngineDLLLifecycle
        "TestEngineDLLLifecycle.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestEngineDLLLifecycle)
    target_link_libraries(TestEngineDLLLifecycle PRIVATE EngineDLL ${CMAKE_DL_LIBS})

    # 20. TestRenderFrameAPI (Phase 5 Track A)
    # Validates the full Editor-facing render API: CreateEntity, CreateCamera, CreateLightSet,
    # CreateRenderSurface, RenderFrame, CaptureBackbuffer, and cleanup via dlopen + dlsym.
    # Includes ShaderCompilation.cpp so we can compile Metal shaders before InitializeEngine,
    # otherwise the engine bails out before any surface/capture path runs.
    add_executable(TestRenderFrameAPI
        "TestRenderFrameAPI.cpp"
        "ShaderCompilation.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRenderFrameAPI)
    target_link_libraries(TestRenderFrameAPI PRIVATE EngineDLL ${CMAKE_DL_LIBS})

    # 21. TestLightCameraAPI (Phase 5 ECS Light/Camera C ABI)
    # Validates AddEntityLight/RemoveEntityLight/Set/Get round-trip via ECS components,
    # plus AddEntityCamera/RemoveEntityCamera/Set/Get round-trip. No device required.
    add_executable(TestLightCameraAPI
        "TestLightCameraAPI.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestLightCameraAPI)
    target_link_libraries(TestLightCameraAPI PRIVATE EngineDLL)

    # 22. TestRenderPipelineAPI (Phase 5 RenderPipeline C ABI + schema-only Bloom/TAA)
    # Validates pass toggles, pass info, settings round-trip, ParamDescriptor table.
    # No engine init needed — uses static pipeline table and pre-init getters.
    add_executable(TestRenderPipelineAPI
        "TestRenderPipelineAPI.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRenderPipelineAPI)
    target_link_libraries(TestRenderPipelineAPI PRIVATE EngineDLL)

    # 23. TestMiscRenderingAPIs (Phase 5 C ABI round-trip for DebugDraw/RenderTarget/PCG)
    # Headless: line queue CRUD, defensive RT cases (no device in test env), PCG lifecycle.
    add_executable(TestMiscRenderingAPIs
        "TestMiscRenderingAPIs.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMiscRenderingAPIs)
    target_link_libraries(TestMiscRenderingAPIs PRIVATE EngineDLL)

    # 24. TestMediatedDataFlow (Phase 5.1 UI-Mediated data flow C ABI)
    # Validates the 14 new APIs: ProceduralMesh, PCG output reads, Asset import,
    # RenderPipeline extensions (RegisterMeshEntity/EditorMode/LumenConfig),
    # and Camera matrix readback. Headless, no GPU/surface.
    add_executable(TestMediatedDataFlow
        "TestMediatedDataFlow.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMediatedDataFlow)
    target_link_libraries(TestMediatedDataFlow PRIVATE EngineDLL)

    # 25. TestGPUMesherIntegration (Phase 9.3a GPU SurfaceNets device validation)
    # Only test that exercises the GPU code path automatically. Requires full device
    # init — pattern after TestRenderFrameAPI (dlopen + dlsym, in-source main()).
    # Sub-tests: RenderBasic, RenderVsCPU, Perf.
    add_executable(TestGPUMesherIntegration
        "TestGPUMesherIntegration.cpp"
        "ShaderCompilation.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestGPUMesherIntegration)
    target_link_libraries(TestGPUMesherIntegration PRIVATE EngineDLL ${CMAKE_DL_LIBS})
    target_compile_definitions(TestGPUMesherIntegration PRIVATE "TEST_GPU_MESHER_INTEGRATION=1")

    # Create IntegrationTests pseudo-target
    add_custom_target(IntegrationTests DEPENDS TestRendererMac TestMetalRenderer TestCSMIntegration TestStandardPipeline TestModularPipeline TestCSMIntegrationRenderGraph TestMultiView TestSponzaRenderGraph TestGeometryDebugSponza TestParticleSponza TestNaniteStreamingPipeline TestHZBStandalone TestForwardRenderer TestPCGScatter TestGeometryAPI TestFieldDrivenScatter TestMaterialPreview TestMaterialPreviewEditor TestRendererRHIDevice TestEngineDLLLifecycle TestRenderFrameAPI TestLightCameraAPI TestRenderPipelineAPI TestMiscRenderingAPIs TestMediatedDataFlow TestGPUMesherIntegration)

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
