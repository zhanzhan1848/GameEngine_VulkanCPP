cmake_minimum_required(VERSION 3.20)

project(IntegrationTests)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
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
        # Xcode 15+ 的 SDK sysroot 不再默认搜索 /usr/local/include，而 Vulkan
        # （MoltenVK）头文件装在那里；显式补上，Vulkan 后端测试才能编译。
        if(EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
            target_include_directories(${target_name} PRIVATE "$ENV{VULKAN_SDK}/Include")
        elseif(EXISTS "/usr/local/include/vulkan/vulkan.h")
            target_include_directories(${target_name} PRIVATE "/usr/local/include")
        endif()
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

    # VT 目标自带依赖(不依赖 ContentTools 目标)
    if(NOT target_name STREQUAL "TestSponzaVT")
        target_link_libraries(${target_name} PRIVATE Engine ContentTools)
    endif()
    
    # stale-test port: per-target POST_BUILD copies of shaders/assets into the shared
    # Darwin/Debug output raced under `ninja -j` ("Error copying directory ... No such
    # file or directory"). All integration targets share that output dir, so a single
    # stamp-based copy target replaces them.
    add_dependencies(${target_name} IntegrationTestsAssetsCopy)
    
    # 注册测试 (使用 Engine_Integration_Tests_ 前缀以便被 run_integration_tests 目标捕获)
    add_test(NAME Engine_Integration_Tests_${target_name} COMMAND ${target_name})
endfunction()

enable_testing()

# stale-test port: one-time (per build) copy of shaders/assets into the shared
# Darwin/Debug output directory used by every integration test target.
set(ITEST_ASSETS_STAMP ${CMAKE_BINARY_DIR}/IntegrationTestsAssetsCopy.stamp)
add_custom_command(OUTPUT ${ITEST_ASSETS_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
        ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets
        ${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/assets
    COMMAND ${CMAKE_COMMAND} -E touch ${ITEST_ASSETS_STAMP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../shaders ${CMAKE_CURRENT_SOURCE_DIR}/../assets
    COMMENT "Copying EngineTest shaders/assets to output directory"
    VERBATIM
)
add_custom_target(IntegrationTestsAssetsCopy DEPENDS ${ITEST_ASSETS_STAMP})

# 所有平台的集成测试源码（RenderItem/ShaderCompilation 等）都调用 ContentTools API，
# BUILD_CONTENT_TOOLS=OFF 时（如无 FBX SDK 的 CI runner）整体跳过，
# 只保留空的 IntegrationTests 聚合目标供 run_integration_tests 等 DEPENDS 引用。
# VT M2.16: Sponza 全材质 VT 化(ForwardSceneRenderer mesh 路径,离屏无窗口)
# 不依赖 ContentTools 目标 — 直接编译 VTBaker.cpp(与单测同模式)
add_executable(TestSponzaVT
    TestSponzaVT.cpp
    ${CMAKE_SOURCE_DIR}/ContentTools/common/VTBaker.cpp
)
setup_test_target(TestSponzaVT)
target_include_directories(TestSponzaVT PRIVATE
    ${CMAKE_SOURCE_DIR}/ContentTools/common
    ${CMAKE_SOURCE_DIR}/third_party/astc-encoder/Source
)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(TestSponzaVT PRIVATE Engine astcenc-native-static ZLIB::ZLIB)
else()
    target_link_libraries(TestSponzaVT PRIVATE Engine astcenc-native-static z)
endif()
target_compile_definitions(TestSponzaVT PRIVATE "ENABLE_VULKAN=1")

if(TARGET ContentTools)
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
    # CardBake.h（经 Geometry.h 传递引入）include 的是 ContentTools/ 根的
    # ToolsCommon.h——setup_test_target 的默认路径不含该目录。
    target_include_directories(TestRendererMac PRIVATE
        ${CMAKE_SOURCE_DIR}/ContentTools)

    # 2. TestMetalRenderer
    add_executable(TestMetalRenderer
        "Main.cpp"
        "TestMetalRenderer.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestMetalRenderer)
    # stale-test port: TestMetalRenderer.cpp implements Engine_Test from TestRenderer.h;
    # without a TEST_* define Main.cpp hits its #error guard.
    target_compile_definitions(TestMetalRenderer PRIVATE "TEST_RENDERER=1")

    # 3. TestCSMIntegration
    add_executable(TestCSMIntegration
        "Main.cpp"
        "TestCSMIntegration.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestCSMIntegration)
    # stale-test port: missing test-selection define made Main.cpp hit its #error guard.
    target_compile_definitions(TestCSMIntegration PRIVATE "TEST_CSM_INTEGRATION=1" "TEST_RENDERER=0")

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

    # 5b. TestMetalGIFusion — GI full-composite alignment verification (Metal).
    # Same sources as TestModularPipeline; TEST_METAL_GI_FUSION switches to the
    # signed-v3 asset, the fixed Vulkan-comparable camera, the ENGINE Metal
    # gather shader, and auto-verifies/exits at frame 90 (GI_VERIFY_FRAME).
    add_executable(TestMetalGIFusion
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
    setup_test_target(TestMetalGIFusion)
    target_compile_definitions(TestMetalGIFusion PRIVATE "TEST_METAL_GI_FUSION=1" "TEST_RENDERER=0")

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

    # 10b. TestVulkanSponzaRenderGraph (T4.6.5 part 30.4)
    # First visible-window Vulkan Sponza render via RenderSystem abstraction.
    # Mirrors TestNaniteStreamingPipeline scaffolding (RenderTestRunner + RenderSystem)
    # but drives StandardRenderPipeline::RenderWithCommandBuffer on a Vulkan device.
    #
    # M0 批次 B（D3D12 F0 门控先在缺陷修复）：以下 Vulkan 集成测试目标只在
    # ENABLE_VULKAN=ON 的构建树（Darwin/DebugVulkan）构建。它们局部定义
    # ENABLE_VULKAN=1 编译 Vulkan 调用代码，但链接的 libEngine.a 只有在
    # ENABLE_VULKAN=ON 时才含 Vulkan 后端符号——Metal-only Debug 树下必然
    # 链接失败。门控模式对齐 UnitTests/CMakeLists.txt 的 TestRHIDeviceFactory
    # 先例（if(ENABLE_VULKAN) → target_compile_definitions ENABLE_VULKAN=1）。
    if(ENABLE_VULKAN)
    add_executable(TestVulkanSponzaRenderGraph
        "Main.cpp"
        "TestVulkanSponzaRenderGraph.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestVulkanSponzaRenderGraph)
    # Diagnostic-only live-SC 2..256-ray witness. Never overwrites production modules.
    find_program(SC_BOUND_WITNESS_GLSLANG NAMES glslangValidator REQUIRED)
    set(SC_BOUND_WITNESS_SPV ${CMAKE_BINARY_DIR}/Tests/SCBoundGeometryWitness.comp.spv)
    add_custom_command(OUTPUT ${SC_BOUND_WITNESS_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Tests
        COMMAND ${SC_BOUND_WITNESS_GLSLANG} -V -I${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen
            ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp -o ${SC_BOUND_WITNESS_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen/GeometryFirstHit.glsl VERBATIM)
    set(SC_BOUND_WITNESS_REPLACE_SPV ${CMAKE_BINARY_DIR}/Tests/SCBoundGeometryWitnessReplace.comp.spv)
    add_custom_command(OUTPUT ${SC_BOUND_WITNESS_REPLACE_SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Tests
        COMMAND ${SC_BOUND_WITNESS_GLSLANG} -V -DDDGI_GEOMETRY_REPLACE -I${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen
            ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp -o ${SC_BOUND_WITNESS_REPLACE_SPV}
        DEPENDS ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp
            ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen/GeometryFirstHit.glsl VERBATIM)
    add_custom_target(SCBoundGeometryWitnessShader DEPENDS ${SC_BOUND_WITNESS_SPV} ${SC_BOUND_WITNESS_REPLACE_SPV})
    add_dependencies(TestVulkanSponzaRenderGraph SCBoundGeometryWitnessShader)
    target_compile_definitions(TestVulkanSponzaRenderGraph PRIVATE SC_BOUND_GEOMETRY_WITNESS_SPV="${SC_BOUND_WITNESS_SPV}" SC_BOUND_GEOMETRY_WITNESS_REPLACE_SPV="${SC_BOUND_WITNESS_REPLACE_SPV}")

    target_compile_definitions(TestVulkanSponzaRenderGraph PRIVATE "TEST_VULKAN_SPONZA_RENDERGRAPH=1" "TEST_RENDERER=0" "ENABLE_VULKAN=1")
    endif() # ENABLE_VULKAN gate for TestVulkanSponzaRenderGraph

    # 10d. TestRHISponzaShared 于 2026-11-03 移出 APPLE 门（原位置）：
    # 见本文件平台 if 链 endif 之后的正式注册块（Windows/ENABLE_D3D12
    # 构建不再因 APPLE 包裹而缺失该测试目标）。



    option(PRIMAL_BUILD_RC_INTEGRATION_TEST "Build isolated Vulkan Sponza RC experiment" OFF)
    if(PRIMAL_BUILD_RC_INTEGRATION_TEST AND ENABLE_VULKAN)
        # Keep opt-in Vulkan archive away from the shared Metal output.
        set_target_properties(Engine PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/RC/lib"
            ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/RC/lib")
        set(RC_SPV ${CMAKE_BINARY_DIR}/Tests/RC/Cascade.comp.spv)
        add_custom_command(OUTPUT ${RC_SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Tests/RC
            COMMAND ${SC_BOUND_WITNESS_GLSLANG} -V -I${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen
                ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/RC/Cascade.comp -o ${RC_SPV}
            DEPENDS ${CMAKE_SOURCE_DIR}/EngineTest/shaders/Vulkan/RC/Cascade.comp
                ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen/SurfaceCacheData.glsl
                ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/Lumen/GeometryFirstHit.glsl VERBATIM)
        add_custom_target(SponzaRCShader DEPENDS ${RC_SPV})
        add_executable(TestVulkanSponzaRC Main.cpp TestVulkanSponzaRenderGraph.cpp MacKeyboard.mm
            ${CMAKE_SOURCE_DIR}/Engine/Input/InputMac.mm ${COMMON_HEADERS})
        setup_test_target(TestVulkanSponzaRC)
        target_include_directories(TestVulkanSponzaRC PRIVATE ${CMAKE_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include)
        get_target_property(RC_BASE_DEFINES TestVulkanSponzaRenderGraph COMPILE_DEFINITIONS)
        target_compile_definitions(TestVulkanSponzaRC PRIVATE ${RC_BASE_DEFINES}
            PRIMAL_RC_INTEGRATION_TEST=1 RC_TEST_SPV="${RC_SPV}")
        add_dependencies(TestVulkanSponzaRC SponzaRCShader SCBoundGeometryWitnessShader)
        set_tests_properties(Engine_Integration_Tests_TestVulkanSponzaRC PROPERTIES
            DISABLED TRUE LABELS "Vulkan;RC;Experimental")
    endif()

    # DLSS 集成门禁测试：复制 Sponza 脚手架 + 内置 FG/NR 自动断言（_Exit 0/1）。
    # 与 TestVulkanSponzaRenderGraph 完全独立——原测试不读 DLSS 门禁 env，
    # 本测试自带 env（Initialize 内 setenv）。运行：
    #   Darwin/Debug/TestVulkanDLSSRenderGraph; echo $?
    # ENABLE_VULKAN 门控（M0 批次 B）：同 10b，只进 Vulkan 构建树。
    if(ENABLE_VULKAN)
    add_executable(TestVulkanDLSSRenderGraph
        "Main.cpp"
        "TestVulkanDLSSRenderGraph.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestVulkanDLSSRenderGraph)
    target_compile_definitions(TestVulkanDLSSRenderGraph PRIVATE "TEST_VULKAN_DLSS_RENDERGRAPH=1" "TEST_RENDERER=0" "ENABLE_VULKAN=1")

    # 10c. TestVulkanFroxelRenderGraph：Froxel 体积雾集成门禁（G1-G4，_Exit
    # 0/1；WP1 后加 G5 双相位）。与 Sponza/DLSS 完全独立——本测试 Initialize
    # 内 setenv(FROXEL=1...)，运行：
    #   Darwin/Debug/TestVulkanFroxelRenderGraph; echo $?
    add_executable(TestVulkanFroxelRenderGraph
        "Main.cpp"
        "TestVulkanFroxelRenderGraph.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
    )
    setup_test_target(TestVulkanFroxelRenderGraph)
    target_compile_definitions(TestVulkanFroxelRenderGraph PRIVATE "TEST_VULKAN_FROXEL_RENDERGRAPH=1" "TEST_RENDERER=0" "ENABLE_VULKAN=1")
    add_dependencies(TestVulkanFroxelRenderGraph VulkanShaderFreshnessGate)
    add_custom_command(TARGET TestVulkanFroxelRenderGraph POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders
        $<TARGET_FILE_DIR:TestVulkanFroxelRenderGraph>/Engine/Graphics/Vulkan/shaders
    )
    endif() # ENABLE_VULKAN gate for TestVulkanDLSSRenderGraph / TestVulkanFroxelRenderGraph

    # Refuse to link/run the Vulkan Sponza target with a source-backed shader
    # older than its GLSL/WGSL input (including recursively included GLSL).
    # This target is deliberately always evaluated: shader sources and the
    # checked-in SPIR-V may be changed outside CMake's generated dependency
    # graph.
    add_custom_target(VulkanShaderFreshnessGate
        COMMAND ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders/build_spv.sh --check-freshness
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking Vulkan shader source/SPIR-V freshness"
        VERBATIM
    )
    if(ENABLE_VULKAN)
    add_dependencies(TestVulkanSponzaRenderGraph VulkanShaderFreshnessGate)

    # T4.6.5 part 35.3: bundle Vulkan SPIR-V shaders next to the test binary.
    # Without this, LoadShaderBytecode falls back to
    # /Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/ + relative path, which
    # resolves to the MAIN repo (feat/wfc-pcg branch) containing stale
    # pre-Part-35.2 .spv files. The relative-path lookup only works if the
    # .spv files exist at <binary_dir>/Engine/Graphics/Vulkan/shaders/...
    add_custom_command(TARGET TestVulkanSponzaRenderGraph POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders
        $<TARGET_FILE_DIR:TestVulkanSponzaRenderGraph>/Engine/Graphics/Vulkan/shaders
    )

    add_custom_command(TARGET TestVulkanDLSSRenderGraph POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders
        $<TARGET_FILE_DIR:TestVulkanDLSSRenderGraph>/Engine/Graphics/Vulkan/shaders
    )
    endif() # ENABLE_VULKAN gate for Vulkan target deps/shader bundles

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

    # 26. TestRenderViewBuilder (Task 1: BuildRenderViewFromCameraId engine helper)
    # Validates invalid-id early-out (no engine required) and the live camera path
    # (requires Metal device + shaders). Gracefully skips the live path if engine
    # init fails, so it can run in CI environments without a GPU.
    add_executable(TestRenderViewBuilder
        "TestRenderViewBuilder.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestRenderViewBuilder)
    target_link_libraries(TestRenderViewBuilder PRIVATE Engine)

    # 27. TestPipelineLightEntity (Task 2: StandardRenderPipeline light entity methods)
    # Validates RegisterLightEntity/UpdateLightEntity/UnregisterLightEntity round-trip
    # through ECS and scene_sync::SyncLightsFromECS. Requires Metal device + shaders.
    # Gracefully skips if engine init fails (CI without GPU).
    add_executable(TestPipelineLightEntity
        "TestPipelineLightEntity.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestPipelineLightEntity)
    target_link_libraries(TestPipelineLightEntity PRIVATE Engine)

    # 28. TestPipelineRenderFrame (Task 7: Path B PipelineRenderFrame E2E)
    # Validates the full headless / offscreen render path via dlopen + dlsym:
    # CreateStandardRenderPipeline -> CreateRenderTarget -> PipelineRenderFrame ->
    # DebugReadRenderTarget -> brightness comparisons + light register/update/unregister.
    # Requires Metal device + shaders; gracefully skips if engine init fails.
    add_executable(TestPipelineRenderFrame
        "TestPipelineRenderFrame.cpp"
        "ShaderCompilation.cpp"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestPipelineRenderFrame)
    target_link_libraries(TestPipelineRenderFrame PRIVATE EngineDLL ${CMAKE_DL_LIBS})

    # 29. TestWFC3DParametric (WFC Phase A.3 Task 11: end-to-end integration test)
    # Headless, no GPU. Exercises the full parametric WFC pipeline:
    #   WFCTileCatalog::Populate -> WFCSolver (multi-tile) -> WFCOutput::ConsumeSteps
    # Wired as a standalone block (mirrors UnitTests/Graphics/WFC/CMakeLists pattern)
    # rather than via setup_test_target, to avoid pulling Cocoa/Metal frameworks and
    # shader-asset POST_BUILD copies that the WFC path does not need.
    add_executable(TestWFC3DParametric
        Graphics/WFC/TestWFC3DParametric.cpp
    )
    target_include_directories(TestWFC3DParametric PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFC3DParametric PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFC3DParametric COMMAND TestWFC3DParametric)
    set_target_properties(TestWFC3DParametric PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFC3DParametric PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 30. TestWFCRendering (WFC Phase A.4 Task 2: Metal window + empty render loop)
    # Mirrors TestPCGScatter pattern: Main.cpp + TestWFCRendering.cpp + MacKeyboard.mm.
    # setup_test_target pulls Cocoa/Metal frameworks and shader-asset POST_BUILD copies
    # that the live Metal pipeline requires (unlike the headless TestWFC3DParametric).
    add_executable(TestWFCRendering
        "Main.cpp"
        "Graphics/WFC/TestWFCRendering.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestWFCRendering)
    target_include_directories(TestWFCRendering PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC
    )
    target_compile_definitions(TestWFCRendering PRIVATE "TEST_WFC_RENDERING=1" "TEST_RENDERER=0")

    # 31. TestWFCRendering2DSmoke (Phase B.1: same sources, WFC_MODE_2D_SMOKE=1 define)
    # CI smoke variant: Initialize() starts in 2D mode (4x4x1 grid) so the 2D
    # collapse path is exercised without interactive input. Mirrors the
    # TestWFCRendering target exactly except for the extra compile definition.
    add_executable(TestWFCRendering2DSmoke
        "Main.cpp"
        "Graphics/WFC/TestWFCRendering.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestWFCRendering2DSmoke)
    target_include_directories(TestWFCRendering2DSmoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC
    )
    target_compile_definitions(TestWFCRendering2DSmoke PRIVATE
        "TEST_WFC_RENDERING=1" "TEST_RENDERER=0" "WFC_MODE_2D_SMOKE=1")

    # 32. TestWFCStreaming (WFC Phase B.2 Task 4: streaming invariant test)
    # Asserts 4x4x4 streaming completes in <240 frames with 64 total collapses.
    # Same scaffolding as TestWFCRendering (Main.cpp + MacKeyboard.mm) so the
    # windowed Metal pipeline is available even though the test doesn't poll keys.
    add_executable(TestWFCStreaming
        "Main.cpp"
        "Graphics/WFC/TestWFCStreaming.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestWFCStreaming)
    target_include_directories(TestWFCStreaming PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC
    )
    target_compile_definitions(TestWFCStreaming PRIVATE "TEST_WFC_STREAMING=1" "TEST_RENDERER=0")

    # 33. TestWFCCategorySolve (WFC Phase C.1 T23: category-mask integration test)
    # Headless, no GPU. Verifies that solving a 4×2×4 grid with
    # active_category_mask=Ruins produces a grid where every collapsed cell
    # holds a Ruins-category tile — proving the T5 PopulateAllCandidates
    # category-mask plumbing flows through end-to-end.
    # Wired as a standalone block mirroring TestWFC3DParametric (block 29).
    add_executable(TestWFCCategorySolve
        Graphics/WFC/TestWFCCategorySolve.cpp
    )
    target_include_directories(TestWFCCategorySolve PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFCCategorySolve PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFCCategorySolve COMMAND TestWFCCategorySolve)
    set_target_properties(TestWFCCategorySolve PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFCCategorySolve PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 34. TestWFCRuinsAdjacencyConsistency (WFC Phase C.1 T24)
    # Headless, no GPU. Verifies every (tile, variant, face) has >= 1 partner
    # so the solver can never dead-lock on a zero-adjacency cell.
    add_executable(TestWFCRuinsAdjacencyConsistency
        Graphics/WFC/TestWFCRuinsAdjacencyConsistency.cpp
    )
    target_include_directories(TestWFCRuinsAdjacencyConsistency PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFCRuinsAdjacencyConsistency PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFCRuinsAdjacencyConsistency
             COMMAND TestWFCRuinsAdjacencyConsistency)
    set_target_properties(TestWFCRuinsAdjacencyConsistency PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFCRuinsAdjacencyConsistency PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 35. TestWFCRuinsSolveSmall (WFC Phase C.1 T25)
    # Headless, no GPU. Small 3×3×3 Ruins-only solve that requires all 27
    # cells to collapse to a Ruins-category tile. Tighter than T23: catches
    # partial-collapse/GivenUp that surfaces only after several cells are
    # already fixed in place (the failure mode T25 exposed in the catalog).
    add_executable(TestWFCRuinsSolveSmall
        Graphics/WFC/TestWFCRuinsSolveSmall.cpp
    )
    target_include_directories(TestWFCRuinsSolveSmall PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFCRuinsSolveSmall PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFCRuinsSolveSmall
             COMMAND TestWFCRuinsSolveSmall)
    set_target_properties(TestWFCRuinsSolveSmall PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFCRuinsSolveSmall PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 36. TestWFCRuinsMixedCategories (WFC Phase C.1 T26)
    # Headless, no GPU. 6×3×6 Primitive+Ruins mixed solve: asserts collapse
    # completes and primitive share lands in [10%, 50%]. Loose smoke test
    # catches catastrophic regressions (cube wildcard removed, ruins-only
    # bias wiping primitives, etc.).
    add_executable(TestWFCRuinsMixedCategories
        Graphics/WFC/TestWFCRuinsMixedCategories.cpp
    )
    target_include_directories(TestWFCRuinsMixedCategories PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFCRuinsMixedCategories PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFCRuinsMixedCategories
             COMMAND TestWFCRuinsMixedCategories)
    set_target_properties(TestWFCRuinsMixedCategories PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFCRuinsMixedCategories PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 37. TestWFCRuinsRestart (WFC Phase C.1 T26)
    # Headless, no GPU. 8×4×8 Ruins-only solve on seed=0xDEAD1: asserts the
    # solver either completes the full 256-cell collapse or restarts ≥ 1
    # generation. Validates RestartPolicy fires under deadlocks.
    add_executable(TestWFCRuinsRestart
        Graphics/WFC/TestWFCRuinsRestart.cpp
    )
    target_include_directories(TestWFCRuinsRestart PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestWFCRuinsRestart PRIVATE Engine)
    add_test(NAME Engine_Integration_Tests_TestWFCRuinsRestart
             COMMAND TestWFCRuinsRestart)
    set_target_properties(TestWFCRuinsRestart PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/IntegrationTests
        FOLDER "EngineTest/IntegrationTests")
    target_compile_definitions(TestWFCRuinsRestart PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)

    # 38. TestWFCRuinsRendering (WFC Phase C.1 T27 visual smoke binary)
    # Live Metal window + StandardRenderPipeline. Boots engine, registers 15
    # catalog meshes (5 primitive + 10 ruins placeholders), runs an 8×4×8
    # ruins-only solver to completion, spawns collapsed cells as ECS entities,
    # renders kHeadlessFrameCap frames, exits. Smoke asserts ≥ 50 cells
    # collapsed; visual quality is human-reviewed.
    add_executable(TestWFCRuinsRendering
        "Main.cpp"
        "Graphics/WFC/TestWFCRuinsRendering.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestWFCRuinsRendering)
    target_include_directories(TestWFCRuinsRendering PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC
    )
    target_compile_definitions(TestWFCRuinsRendering PRIVATE "TEST_WFC_RUINS_RENDERING=1" "TEST_RENDERER=0")

    # 39. TestKenneyTilePreview (Kenney dungeon tile asset-pack preview)
    # Live Metal window + StandardRenderPipeline. Loads every .engine_mesh in
    # EngineTest/assets/Processed/kenney_dungeon_tiles/ via KenneyTileCatalog
    # (application-layer loader, NOT engine core), places tiles in an 8×5 grid
    # at cell_size=4m, spawns ECS entities via PCGEntityFactory, renders
    # kHeadlessFrameCap=120 frames, exits. Smoke asserts ≥ 1 tile loaded AND
    # spawned; visual quality is human-reviewed.
    add_executable(TestKenneyTilePreview
        "Main.cpp"
        "Graphics/WFC/TestKenneyTilePreview.cpp"
        "Graphics/WFC/KenneyTileCatalog.cpp"
        "Graphics/WFC/Styles/KenneyDungeonStyle.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestKenneyTilePreview)
    target_include_directories(TestKenneyTilePreview PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/WFC/Styles
    )
    target_compile_definitions(TestKenneyTilePreview PRIVATE "TEST_KENNEY_TILE_PREVIEW=1" "TEST_RENDERER=0")

    # 34. TestPhysicsSmoke — T0 物理可视化冒烟（Engine/Simulation × Jolt）。
    # 场景/断言/PNG 产物见文件头与 Docs/2026-09-11-physics-simulation-design.md §8.1。
    # 脚手架沿 TestWFCRendering（窗口 + 前向管线 + 帧数上限退出）；
    # 渲染目标为离屏 RGBA8 RT，帧 {1,30,60,90,120} 落 PNG 到 test-output/physics-smoke/。
    add_executable(TestPhysicsSmoke
        "Main.cpp"
        "TestPhysicsSmoke.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestPhysicsSmoke)
    target_compile_definitions(TestPhysicsSmoke PRIVATE "TEST_PHYSICS_SMOKE=1" "TEST_RENDERER=0")

    # 35. TestPhysicsMultiBody — T1 多物体刚体可视化测试（M1，设计文档 §8.2）。
    # 三场景：砖塔安定+推倒 / 金字塔坍塌 / 弹性阶梯。确定性哈希基线 +
    # 双管线可视化（窗口 RWC backbuffer + 捕获管线离屏 RT PNG）。
    add_executable(TestPhysicsMultiBody
        "Main.cpp"
        "TestPhysicsMultiBody.cpp"
        "MacKeyboard.mm"
        ${COMMON_HEADERS}
    )
    setup_test_target(TestPhysicsMultiBody)
    target_compile_definitions(TestPhysicsMultiBody PRIVATE "TEST_PHYSICS_MULTIBODY=1" "TEST_RENDERER=0")

    # Create IntegrationTests pseudo-target
    add_custom_target(IntegrationTests DEPENDS TestRendererMac TestMetalRenderer TestCSMIntegration TestStandardPipeline TestModularPipeline TestCSMIntegrationRenderGraph TestMultiView TestSponzaRenderGraph TestGeometryDebugSponza TestParticleSponza TestNaniteStreamingPipeline TestHZBStandalone TestForwardRenderer TestPCGScatter TestGeometryAPI TestFieldDrivenScatter TestMaterialPreview TestMaterialPreviewEditor TestRendererRHIDevice TestEngineDLLLifecycle TestRenderFrameAPI TestLightCameraAPI TestRenderPipelineAPI TestMiscRenderingAPIs TestMediatedDataFlow TestGPUMesherIntegration TestRenderViewBuilder TestPipelineLightEntity TestPipelineRenderFrame TestWFC3DParametric TestWFCRendering TestWFCRendering2DSmoke TestWFCStreaming TestWFCCategorySolve TestWFCRuinsAdjacencyConsistency TestWFCRuinsSolveSmall TestWFCRuinsMixedCategories TestWFCRuinsRestart TestWFCRuinsRendering TestKenneyTilePreview TestPhysicsSmoke TestPhysicsMultiBody)

    # 12. TestDawnRenderer (WebGPU)
    if(ENABLE_WEBGPU AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/dawn)
        set(DAWN_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/dawn)
        add_executable(TestDawnRenderer
            "TestDawnMain.cpp"
            "TestDawnRenderer.cpp"
            ${COMMON_HEADERS}
        )
        setup_test_target(TestDawnRenderer)
        target_compile_definitions(TestDawnRenderer PRIVATE "TEST_DAWN_RENDERER=1" "ENABLE_WEBGPU=1")
        target_include_directories(TestDawnRenderer PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RHI/Platforms/Dawn"
            "${DAWN_ROOT_DIR}/include"
            "${DAWN_ROOT_DIR}/src/include"
            "${DAWN_ROOT_DIR}/gen"
        )
        target_link_libraries(TestDawnRenderer PRIVATE webgpu_dawn)
        add_dependencies(IntegrationTests TestDawnRenderer)

        # 13. TestDawnRenderingWindow (WebGPU windowed rendering)
        add_executable(TestDawnRenderingWindow
            "Main.cpp"
            "TestDawnRendering.cpp"
            "MacKeyboard.mm"
            ${COMMON_HEADERS}
        )
        setup_test_target(TestDawnRenderingWindow)
        target_compile_definitions(TestDawnRenderingWindow PRIVATE "TEST_DAWN_RENDERING=1" "ENABLE_WEBGPU=1")
        target_include_directories(TestDawnRenderingWindow PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RHI/Platforms/Dawn"
            "${DAWN_ROOT_DIR}/include"
            "${DAWN_ROOT_DIR}/src/include"
            "${DAWN_ROOT_DIR}/gen"
        )
        target_link_libraries(TestDawnRenderingWindow PRIVATE webgpu_dawn)
        add_dependencies(IntegrationTests TestDawnRenderingWindow)

        # 14. TestDawnSponza (WebGPU Sponza rendering)
        add_executable(TestDawnSponza
            "Main.cpp"
            "TestDawnSponza.cpp"
            "MacKeyboard.mm"
            ${COMMON_HEADERS}
        )
        setup_test_target(TestDawnSponza)
        target_compile_definitions(TestDawnSponza PRIVATE "TEST_DAWN_SPONZA=1" "ENABLE_WEBGPU=1")
        target_include_directories(TestDawnSponza PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RHI/Platforms/Dawn"
            "${DAWN_ROOT_DIR}/include"
            "${DAWN_ROOT_DIR}/src/include"
            "${DAWN_ROOT_DIR}/gen"
        )
        target_link_libraries(TestDawnSponza PRIVATE webgpu_dawn)
        add_dependencies(IntegrationTests TestDawnSponza)

        # 15. TestDawnRenderGraph (WebGPU RenderGraph integration)
        add_executable(TestDawnRenderGraph
            "Main.cpp"
            "TestDawnRenderGraph.cpp"
            "MacKeyboard.mm"
            ${COMMON_HEADERS}
        )
        setup_test_target(TestDawnRenderGraph)
        target_compile_definitions(TestDawnRenderGraph PRIVATE "TEST_DAWN_RENDERGRAPH=1" "ENABLE_WEBGPU=1")
        target_include_directories(TestDawnRenderGraph PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RHI/Platforms/Dawn"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderGraph"
            "${DAWN_ROOT_DIR}/include"
            "${DAWN_ROOT_DIR}/src/include"
            "${DAWN_ROOT_DIR}/gen"
        )
        target_link_libraries(TestDawnRenderGraph PRIVATE webgpu_dawn)
        add_dependencies(IntegrationTests TestDawnRenderGraph)

        # 16. TestDawnForwardRenderer (ForwardRenderer on Dawn)
        add_executable(TestDawnForwardRenderer
            "Main.cpp"
            "TestDawnForwardRenderer.cpp"
            "MacKeyboard.mm"
            ${COMMON_HEADERS}
        )
        setup_test_target(TestDawnForwardRenderer)
        target_compile_definitions(TestDawnForwardRenderer PRIVATE "TEST_DAWN_FORWARD_RENDERER=1" "ENABLE_WEBGPU=1" "$<$<BOOL:${DISABLE_PARTICLE_SYSTEM}>:DISABLE_PARTICLE_SYSTEM=1>")
        target_include_directories(TestDawnForwardRenderer PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RHI/Platforms/Dawn"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Graphics/RenderGraph"
            "${DAWN_ROOT_DIR}/include"
            "${DAWN_ROOT_DIR}/src/include"
            "${DAWN_ROOT_DIR}/gen"
        )
        target_link_libraries(TestDawnForwardRenderer PRIVATE webgpu_dawn)
        add_dependencies(IntegrationTests TestDawnForwardRenderer)
    endif()

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

# 10d. TestRHISponzaShared (D3D12 M1 F1-8 共享测试宿主) —— 2026-11-03 移出
# APPLE 门：注册条件 = ENABLE_VULKAN（任意平台）。参数化 platform（env
# RHI_TEST_PLATFORM=vulkan|d3d12）：同一份中立 RHI 调用代码驱动 Sponza
# forward 轨迹（M1Forward 管线、离屏 readback PPM、resize 段、资源账本）。
# 本文件零后端 include/cast（F0-12 门禁）——d3d12 在无 D3D12 后端的构建上
# 由工厂返回 nullptr → SKIP exit 42。宿主离屏无窗口依赖：macOS 只进
# DebugVulkan 树（Metal 树 ENABLE_VULKAN=OFF 不生成）；Windows 双后端构建
# （ENABLE_VULKAN+ENABLE_D3D12）生成同一二进制，d3d12 侧运行时切换。
# 仍在 TARGET ContentTools 门内：setup_test_target 链接 Engine+ContentTools。
if(ENABLE_VULKAN)
    add_executable(TestRHISponzaShared
        "TestRHISponzaShared.cpp"
    )
    setup_test_target(TestRHISponzaShared)
    target_compile_definitions(TestRHISponzaShared PRIVATE "ENABLE_VULKAN=1")
    # ctest 入口 = 冒烟帧数版（3 帧，独立 capture 目录，不污染正式证据目录）；
    # 完整 240 帧轨迹由 D3D12 M1 交付流程显式 env 驱动（见 PhaseDoc §5）。
    set_tests_properties(Engine_Integration_Tests_TestRHISponzaShared PROPERTIES
        TIMEOUT 300 LABELS "Vulkan;RHI;D3D12M1"
        ENVIRONMENT "RHI_TEST_FRAMES=3;RHI_CAPTURE_DIR=/tmp/d3d12_m1_evidence/ctest_smoke_frames")
endif() # ENABLE_VULKAN gate for TestRHISponzaShared (platform-agnostic)

else()
    message(STATUS "Integration tests skipped: ContentTools disabled (BUILD_CONTENT_TOOLS=OFF)")
    add_custom_target(IntegrationTests)
endif()
