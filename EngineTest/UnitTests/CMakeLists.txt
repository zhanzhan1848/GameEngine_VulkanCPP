cmake_minimum_required(VERSION 3.20)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置测试输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests)

# ======================
# 独立RHI测试目标 - 不依赖任何外部库
# ======================

# 创建RHI类型系统测试
add_executable(StandaloneRHI_Test RHI/Core/TestRHITypes.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Test COMMAND StandaloneRHI_Test)
set_target_properties(StandaloneRHI_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Test PRIVATE DEBUG _DEBUG)

# 创建RHI设备管理测试
add_executable(StandaloneRHI_Device_Test RHI/Core/TestRHIDevice.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Device_Test COMMAND StandaloneRHI_Device_Test)
set_target_properties(StandaloneRHI_Device_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Device_Test PRIVATE DEBUG _DEBUG)

# 创建RHI设备工厂测试（端到端验证 CreateRHIDevice/DestroyRHIDevice）
add_executable(TestRHIDeviceFactory RHI/Core/TestRHIDeviceFactory.cpp)
target_include_directories(TestRHIDeviceFactory PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)
target_link_libraries(TestRHIDeviceFactory PRIVATE
    Engine
    "-framework Foundation"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
)
add_test(NAME TestRHIDeviceFactory COMMAND TestRHIDeviceFactory)
set_target_properties(TestRHIDeviceFactory PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)

# 创建RHI资源管理测试
add_executable(StandaloneRHI_Resource_Test RHI/Core/TestRHIResource.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Resource_Test COMMAND StandaloneRHI_Resource_Test)
set_target_properties(StandaloneRHI_Resource_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Resource_Test PRIVATE DEBUG _DEBUG)

# 创建RHI自适应内存池测试
add_executable(StandaloneRHI_AdaptiveMemoryPool_Test 
    RHI/Core/TestRHIAdaptiveMemoryPool.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIAdaptiveMemoryPool.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIMemoryPool.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_AdaptiveMemoryPool_Test COMMAND StandaloneRHI_AdaptiveMemoryPool_Test)
set_target_properties(StandaloneRHI_AdaptiveMemoryPool_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_include_directories(StandaloneRHI_AdaptiveMemoryPool_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_compile_definitions(StandaloneRHI_AdaptiveMemoryPool_Test PRIVATE DEBUG _DEBUG)

# 创建哈希基准测试
add_executable(HashBenchmark RHI/Core/HashBenchmark.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME HashBenchmark COMMAND HashBenchmark)
set_target_properties(HashBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)

# 创建RHI批渲染提交测试
add_executable(StandaloneRHI_BatchRenderer_Test RHI/Core/TestRHIBatchRenderer.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_BatchRenderer_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 链接Engine库以获取RHIBatchRenderer实现
target_link_libraries(StandaloneRHI_BatchRenderer_Test PRIVATE Engine)
add_test(NAME StandaloneRHI_BatchRenderer_Test COMMAND StandaloneRHI_BatchRenderer_Test)
set_target_properties(StandaloneRHI_BatchRenderer_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_BatchRenderer_Test PRIVATE DEBUG _DEBUG)

# 创建RHI确定性预取测试
add_executable(StandaloneRHI_DeterministicPrefetch_Test RHI/Core/TestRHIDeterministicPrefetch.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_DeterministicPrefetch_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 链接Engine库以获取RHIDeterministicPrefetch实现
target_link_libraries(StandaloneRHI_DeterministicPrefetch_Test PRIVATE Engine)
add_test(NAME StandaloneRHI_DeterministicPrefetch_Test COMMAND StandaloneRHI_DeterministicPrefetch_Test)
set_target_properties(StandaloneRHI_DeterministicPrefetch_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_DeterministicPrefetch_Test PRIVATE DEBUG _DEBUG)

# 创建ShadowUtils测试
add_executable(StandaloneRHI_ShadowUtils_Test 
    Graphics/TestShadowUtils.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Utils/ShadowUtils.h
)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_ShadowUtils_Test COMMAND StandaloneRHI_ShadowUtils_Test)
set_target_properties(StandaloneRHI_ShadowUtils_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_include_directories(StandaloneRHI_ShadowUtils_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_compile_definitions(StandaloneRHI_ShadowUtils_Test PRIVATE DEBUG _DEBUG)

# 创建 Spherical Harmonics 测试
add_executable(TestSphericalHarmonics Graphics/TestSphericalHarmonics.cpp)
add_test(NAME TestSphericalHarmonics COMMAND TestSphericalHarmonics)
set_target_properties(TestSphericalHarmonics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_include_directories(TestSphericalHarmonics PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_compile_definitions(TestSphericalHarmonics PRIVATE DEBUG _DEBUG)

# 创建 PRT Baker 测试
add_executable(TestPRTBaker Graphics/TestPRTBaker.cpp)
target_include_directories(TestPRTBaker PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(TestPRTBaker PRIVATE Engine)
add_test(NAME TestPRTBaker COMMAND TestPRTBaker)
set_target_properties(TestPRTBaker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestPRTBaker PRIVATE DEBUG _DEBUG)

# 创建SamplingUtils测试
add_executable(StandaloneRHI_SamplingUtils_Test Graphics/TestSamplingUtils.cpp)
target_include_directories(StandaloneRHI_SamplingUtils_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest
)
target_link_libraries(StandaloneRHI_SamplingUtils_Test PRIVATE Engine)
add_test(NAME StandaloneRHI_SamplingUtils_Test COMMAND StandaloneRHI_SamplingUtils_Test)
set_target_properties(StandaloneRHI_SamplingUtils_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_SamplingUtils_Test PRIVATE DEBUG _DEBUG)


# 创建Metal数学库测试
if(APPLE)
    add_executable(TestMetalMath Graphics/Metal/TestMetalMath.cpp)
    target_include_directories(TestMetalMath PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    # MetalMath是header-only的，不需要链接Metal框架，只需要Foundation等

    # 创建Metal Pipeline测试
    add_executable(TestMetalPipeline Graphics/Metal/TestMetalPipeline.cpp)
    target_include_directories(TestMetalPipeline PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    # 链接 Engine 库 (包含 Metal RHI 实现)
    target_link_libraries(TestMetalPipeline PRIVATE Engine)
    add_test(NAME TestMetalPipeline COMMAND TestMetalPipeline)
    set_target_properties(TestMetalPipeline PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    target_link_libraries(TestMetalMath PRIVATE 
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalMath COMMAND TestMetalMath)
    set_target_properties(TestMetalMath PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal设备测试
    add_executable(TestMetalDevice Graphics/Metal/TestMetalDevice.cpp)
    target_include_directories(TestMetalDevice PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalDevice PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalDevice COMMAND TestMetalDevice)
    set_target_properties(TestMetalDevice PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal资源测试
    add_executable(TestMetalResource Graphics/Metal/TestMetalResource.cpp)
    target_include_directories(TestMetalResource PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalResource PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalResource COMMAND TestMetalResource)
    set_target_properties(TestMetalResource PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal纹理测试
    add_executable(TestMetalTexture Graphics/Metal/TestMetalTexture.cpp)
    target_include_directories(TestMetalTexture PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalTexture PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalTexture COMMAND TestMetalTexture)
    set_target_properties(TestMetalTexture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal命令缓冲区测试
    add_executable(TestMetalCommandBuffer Graphics/Metal/TestMetalCommandBuffer.cpp)
    target_include_directories(TestMetalCommandBuffer PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalCommandBuffer PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalCommandBuffer COMMAND TestMetalCommandBuffer)
    set_target_properties(TestMetalCommandBuffer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal资源绑定测试
    add_executable(TestMetalResourceBinding Graphics/Metal/TestMetalResourceBinding.cpp)
    target_include_directories(TestMetalResourceBinding PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalResourceBinding PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalResourceBinding COMMAND TestMetalResourceBinding)
    set_target_properties(TestMetalResourceBinding PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal同步和查询测试
    add_executable(TestMetalSyncQuery Graphics/Metal/TestMetalSyncQuery.cpp)
    target_include_directories(TestMetalSyncQuery PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalSyncQuery PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalSyncQuery COMMAND TestMetalSyncQuery)
    set_target_properties(TestMetalSyncQuery PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # PR4 Spike: encoder-level memoryBarrier validation
    add_executable(TestMetalEncoderBarrierSpike Graphics/Metal/TestMetalEncoderBarrierSpike.cpp)
    target_include_directories(TestMetalEncoderBarrierSpike PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalEncoderBarrierSpike PRIVATE
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalEncoderBarrierSpike COMMAND TestMetalEncoderBarrierSpike)
    set_target_properties(TestMetalEncoderBarrierSpike PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal混合状态测试
    add_executable(TestMetalBlendState Graphics/Metal/TestMetalBlendState.cpp)
    target_include_directories(TestMetalBlendState PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalBlendState PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalBlendState COMMAND TestMetalBlendState)
    set_target_properties(TestMetalBlendState PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal交换链测试
    add_executable(TestMetalSwapChain Graphics/Metal/TestMetalSwapChain.cpp)
    target_include_directories(TestMetalSwapChain PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalSwapChain PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalSwapChain COMMAND TestMetalSwapChain)
    set_target_properties(TestMetalSwapChain PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal着色器和管线测试
    add_executable(TestMetalShaderPipeline Graphics/Metal/TestMetalShaderPipeline.cpp)
    target_include_directories(TestMetalShaderPipeline PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalShaderPipeline PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalShaderPipeline COMMAND TestMetalShaderPipeline)
    set_target_properties(TestMetalShaderPipeline PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建 Metal Descriptor Set 测试
    add_executable(TestMetalDescriptorSet Graphics/Metal/TestMetalDescriptorSet.cpp)
    target_include_directories(TestMetalDescriptorSet PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalDescriptorSet PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    add_test(NAME TestMetalDescriptorSet COMMAND TestMetalDescriptorSet)
    set_target_properties(TestMetalDescriptorSet PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建Metal命令编码测试
    add_executable(TestMetalCommandEncoding Graphics/Metal/TestMetalCommandEncoding.cpp)
    target_include_directories(TestMetalCommandEncoding PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalCommandEncoding PRIVATE Engine "-framework Foundation" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    add_test(NAME TestMetalCommandEncoding COMMAND TestMetalCommandEncoding)
    set_target_properties(TestMetalCommandEncoding PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    target_compile_definitions(TestMetalCommandEncoding PRIVATE DEBUG _DEBUG)

    # 创建 Metal Shader 熱重载测试
    add_executable(TestMetalHotReload Graphics/Metal/TestMetalHotReload.cpp)
    target_include_directories(TestMetalHotReload PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalHotReload PRIVATE Engine "-framework Foundation" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    add_test(NAME TestMetalHotReload COMMAND TestMetalHotReload)
    set_target_properties(TestMetalHotReload PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    target_compile_definitions(TestMetalHotReload PRIVATE DEBUG _DEBUG)

    # 创建 ShaderIR + MaterialGraphToIR 测试
    add_executable(TestShaderIR Graphics/TestShaderIR.cpp)
    target_include_directories(TestShaderIR PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
    )
    target_link_libraries(TestShaderIR PRIVATE Engine "-framework Foundation" "-framework Metal" "-framework QuartzCore")
    add_test(NAME TestShaderIR COMMAND TestShaderIR)
    set_target_properties(TestShaderIR PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    target_compile_definitions(TestShaderIR PRIVATE DEBUG _DEBUG)

    # 创建 IBL Shader 测试
    add_executable(StandaloneGraphics_IBL_Test Graphics/TestIBLShaders.cpp)
    target_include_directories(StandaloneGraphics_IBL_Test PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_compile_definitions(StandaloneGraphics_IBL_Test PRIVATE DEBUG _DEBUG STANDALONE_TEST PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME StandaloneGraphics_IBL_Test COMMAND StandaloneGraphics_IBL_Test)
    set_target_properties(StandaloneGraphics_IBL_Test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    set_target_properties(StandaloneGraphics_IBL_Test PROPERTIES FOLDER "Tests/Graphics")

    # 创建Material测试
    add_executable(StandaloneGraphics_Material_Test 
        Graphics/TestMaterial.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Material.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/MaterialInstance.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    )
    target_include_directories(StandaloneGraphics_Material_Test PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_compile_definitions(StandaloneGraphics_Material_Test PRIVATE DEBUG _DEBUG STANDALONE_TEST PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME StandaloneGraphics_Material_Test COMMAND StandaloneGraphics_Material_Test)
    set_target_properties(StandaloneGraphics_Material_Test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    set_target_properties(StandaloneGraphics_Material_Test PROPERTIES FOLDER "Tests/Graphics")

    # 创建 RHISceneSynchronizer 测试
    add_executable(TestRHISceneSynchronizer
        Graphics/TestRHISceneSynchronizer.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Systems/RHISceneSynchronizer.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIEntityManager.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/RenderLayerComponent.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/MaterialComponent.cpp
    )
    target_include_directories(TestRHISceneSynchronizer PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    )
    target_link_libraries(TestRHISceneSynchronizer PRIVATE Engine)
    target_compile_definitions(TestRHISceneSynchronizer PRIVATE DEBUG _DEBUG)
    add_test(NAME TestRHISceneSynchronizer COMMAND TestRHISceneSynchronizer)
    set_target_properties(TestRHISceneSynchronizer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    set_target_properties(TestRHISceneSynchronizer PROPERTIES FOLDER "Tests/Graphics")
    
    # 创建 Metal PRT 测试
    add_executable(TestMetalPRT Graphics/Metal/TestMetalPRT.cpp)
    target_include_directories(TestMetalPRT PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestMetalPRT PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    target_compile_definitions(TestMetalPRT PRIVATE 
        DEBUG 
        _DEBUG 
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME TestMetalPRT COMMAND TestMetalPRT)
    set_target_properties(TestMetalPRT PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )


    #### Nanite 单元测试 ####

    # 创建 Nanite Resource 测试
    add_executable(TestNaniteResourceManager Nanite/TestNaniteResourceManager.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Nanite/NaniteResourceManager.cpp
    )
    target_include_directories(TestNaniteResourceManager PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestNaniteResourceManager PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    target_compile_definitions(TestNaniteResourceManager PRIVATE 
        DEBUG 
        _DEBUG 
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME TestNaniteResourceManager COMMAND TestNaniteResourceManager)
    set_target_properties(TestNaniteResourceManager PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建 Nanite Streaming Manager 测试
    add_executable(TestNaniteStreamingManager Graphics/TestNaniteStreamingManager.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Nanite/NaniteStreamingManager.cpp
    )
    target_include_directories(TestNaniteStreamingManager PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestNaniteStreamingManager PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    target_compile_definitions(TestNaniteStreamingManager PRIVATE 
        DEBUG 
        _DEBUG 
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME TestNaniteStreamingManager COMMAND TestNaniteStreamingManager)
    set_target_properties(TestNaniteStreamingManager PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建 Cluster Component 测试
    add_executable(TestClusterComponent Components/TestClusterComponent.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Components/Cluster.cpp
    )
    target_include_directories(TestClusterComponent PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestClusterComponent PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    target_compile_definitions(TestClusterComponent PRIVATE 
        DEBUG 
        _DEBUG 
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME TestClusterComponent COMMAND TestClusterComponent)
    set_target_properties(TestClusterComponent PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    # 创建 Scene Snapshot 测试
    add_executable(TestSceneSnapshot Graphics/TestSceneSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Scene/RenderSceneSnapshot.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Graphics/Scene/SceneExtractionSystem.cpp
    )
    target_include_directories(TestSceneSnapshot PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
        ${CMAKE_SOURCE_DIR}/Engine/Common
        ${CMAKE_SOURCE_DIR}/OSAPI/MAC
    )
    target_link_libraries(TestSceneSnapshot PRIVATE 
        Engine
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )
    target_compile_definitions(TestSceneSnapshot PRIVATE 
        DEBUG 
        _DEBUG 
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME TestSceneSnapshot COMMAND TestSceneSnapshot)
    set_target_properties(TestSceneSnapshot PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )

    ####

endif()

# RHI GPU优化测试
add_executable(StandaloneRHI_GPUOptimizer_Test RHI/Core/TestRHIGPUOptimizer.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_GPUOptimizer_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# 链接Engine库以获取RHIGPUOptimizer实现
target_link_libraries(StandaloneRHI_GPUOptimizer_Test PRIVATE Engine)
add_test(NAME StandaloneRHI_GPUOptimizer_Test COMMAND StandaloneRHI_GPUOptimizer_Test)
set_target_properties(StandaloneRHI_GPUOptimizer_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_GPUOptimizer_Test PRIVATE DEBUG _DEBUG)

# 创建RHI多线程命令生成器测试
add_executable(StandaloneRHI_MultiThreadedCommandGenerator_Test RHI/Core/TestRHIMultiThreadedCommandGenerator.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_MultiThreadedCommandGenerator_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# 链接Engine库以获取RHIMultiThreadedCommandGenerator实现
target_link_libraries(StandaloneRHI_MultiThreadedCommandGenerator_Test PRIVATE Engine)
# 添加pthread支持用于多线程测试
target_link_libraries(StandaloneRHI_MultiThreadedCommandGenerator_Test PRIVATE pthread)
add_test(NAME StandaloneRHI_MultiThreadedCommandGenerator_Test COMMAND StandaloneRHI_MultiThreadedCommandGenerator_Test)
set_target_properties(StandaloneRHI_MultiThreadedCommandGenerator_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_MultiThreadedCommandGenerator_Test PRIVATE DEBUG _DEBUG)

# 创建RHI分配器测试
add_executable(StandaloneRHI_Allocator_Test RHI/Core/TestRHIAllocator.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_Allocator_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 链接Engine库
target_link_libraries(StandaloneRHI_Allocator_Test PRIVATE Engine pthread)
add_test(NAME StandaloneRHI_Allocator_Test COMMAND StandaloneRHI_Allocator_Test)
set_target_properties(StandaloneRHI_Allocator_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Allocator_Test PRIVATE DEBUG _DEBUG)

# 创建RHI垃圾回收器测试
add_executable(StandaloneRHI_GarbageCollector_Test RHI/Core/TestRHIGarbageCollector.cpp)
# 添加Engine头文件包含路径
target_include_directories(StandaloneRHI_GarbageCollector_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 链接Engine库
target_link_libraries(StandaloneRHI_GarbageCollector_Test PRIVATE Engine pthread)
add_test(NAME StandaloneRHI_GarbageCollector_Test COMMAND StandaloneRHI_GarbageCollector_Test)
set_target_properties(StandaloneRHI_GarbageCollector_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_GarbageCollector_Test PRIVATE DEBUG _DEBUG)


# 创建RHI命令队列测试
add_executable(StandaloneRHI_Queue_Test RHI/Core/TestRHIQueue.cpp)
# 不依赖任何Engine头文件或库，完全独立
add_test(NAME StandaloneRHI_Queue_Test COMMAND StandaloneRHI_Queue_Test)
set_target_properties(StandaloneRHI_Queue_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Queue_Test PRIVATE DEBUG _DEBUG)

# 创建RenderMesh测试
add_executable(StandaloneGraphics_RenderMesh_Test 
    Graphics/TestRenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(StandaloneGraphics_RenderMesh_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderMesh_Test COMMAND StandaloneGraphics_RenderMesh_Test)
set_target_properties(StandaloneGraphics_RenderMesh_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderMesh_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderMesh_Test PROPERTIES FOLDER "Tests/Graphics")

# 创建 Pipeline Component 测试
add_executable(TestPipelineComponent Components/TestPipelineComponent.cpp)
target_include_directories(TestPipelineComponent PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(TestPipelineComponent PRIVATE Engine)
add_test(NAME TestPipelineComponent COMMAND TestPipelineComponent)
set_target_properties(TestPipelineComponent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestPipelineComponent PRIVATE DEBUG _DEBUG)

# 创建 CommandBuffer Component 测试
add_executable(TestCommandBufferComponent Components/TestCommandBufferComponent.cpp)
target_include_directories(TestCommandBufferComponent PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(TestCommandBufferComponent PRIVATE Engine)
add_test(NAME TestCommandBufferComponent COMMAND TestCommandBufferComponent)
set_target_properties(TestCommandBufferComponent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestCommandBufferComponent PRIVATE DEBUG _DEBUG)


# 创建RenderTexture测试
add_executable(StandaloneGraphics_RenderTexture_Test 
    Graphics/TestRenderTexture.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderTexture.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
# 添加Engine头文件包含路径
target_include_directories(StandaloneGraphics_RenderTexture_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 不链接Engine库，使用源码编译以确保符号可见
# target_link_libraries(StandaloneGraphics_RenderTexture_Test PRIVATE Engine)
add_test(NAME StandaloneGraphics_RenderTexture_Test COMMAND StandaloneGraphics_RenderTexture_Test)
set_target_properties(StandaloneGraphics_RenderTexture_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderTexture_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderTexture_Test PROPERTIES FOLDER "Tests/Graphics")

# 创建RenderProxy测试
add_executable(StandaloneGraphics_RenderProxy_Test 
    Graphics/TestRenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_RenderProxy_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderProxy_Test COMMAND StandaloneGraphics_RenderProxy_Test)
set_target_properties(StandaloneGraphics_RenderProxy_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderProxy_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderProxy_Test PROPERTIES FOLDER "Tests/Graphics")

# 创建RenderScene测试
add_executable(StandaloneGraphics_RenderScene_Test 
    Graphics/TestRenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_RenderScene_Test PRIVATE    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderScene_Test COMMAND StandaloneGraphics_RenderScene_Test)
set_target_properties(StandaloneGraphics_RenderScene_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderScene_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderScene_Test PROPERTIES FOLDER "Tests/Graphics")

# Create RenderScene Light Test
add_executable(StandaloneGraphics_RenderSceneLight_Test 
    Graphics/TestRenderSceneLight.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_RenderSceneLight_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderSceneLight_Test COMMAND StandaloneGraphics_RenderSceneLight_Test)
set_target_properties(StandaloneGraphics_RenderSceneLight_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderSceneLight_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderSceneLight_Test PROPERTIES FOLDER "Tests/Graphics")

# 创建RenderView测试
add_executable(StandaloneGraphics_RenderView_Test 
    Graphics/TestRenderView.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderView.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_RenderView_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderView_Test COMMAND StandaloneGraphics_RenderView_Test)
set_target_properties(StandaloneGraphics_RenderView_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderView_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderView_Test PROPERTIES FOLDER "Tests/Graphics")

# 创建RenderSystem测试
add_executable(StandaloneGraphics_RenderSystem_Test 
    Graphics/TestRenderSystem.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Systems/RenderSystem.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/ForwardRenderer.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Passes/BlurPass.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Utils/ShadowUtils.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderView.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Material.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/MaterialInstance.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHISwapChain.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_RenderSystem_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneGraphics_RenderSystem_Test COMMAND StandaloneGraphics_RenderSystem_Test)
set_target_properties(StandaloneGraphics_RenderSystem_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_RenderSystem_Test PRIVATE DEBUG _DEBUG)
set_target_properties(StandaloneGraphics_RenderSystem_Test PROPERTIES FOLDER "Tests/Graphics")


# 创建 RenderGraph 测试
add_executable(TestRenderGraph 
    Graphics/RenderGraph/TestRenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraphBuilder.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHISwapChain.cpp
)
target_include_directories(TestRenderGraph PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestRenderGraph COMMAND TestRenderGraph)
set_target_properties(TestRenderGraph PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRenderGraph PRIVATE DEBUG _DEBUG)
set_target_properties(TestRenderGraph PROPERTIES FOLDER "Tests/Graphics")


# 创建 RenderGraphDebug 测试
add_executable(TestRenderGraphDebug 
    Graphics/RenderGraph/TestRenderGraphDebug.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraphDebug.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraphBuilder.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHISwapChain.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Input/Input.cpp
)
target_include_directories(TestRenderGraphDebug PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestRenderGraphDebug COMMAND TestRenderGraphDebug)
set_target_properties(TestRenderGraphDebug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRenderGraphDebug PRIVATE DEBUG _DEBUG)
set_target_properties(TestRenderGraphDebug PROPERTIES FOLDER "Tests/Graphics")




# 设置moodycamel库路径
set(MOODYCAMEL_ROOT ${CMAKE_SOURCE_DIR}/third_party/moodycamel-ConcurrentQueue)
set(MOODYCAMEL_INCLUDE_DIRS ${MOODYCAMEL_ROOT})

# MPSC队列简化验证测试 (符合技术规范)
add_executable(MpscQueueSimpleTest MpscQueueSimpleTest.cpp)
# 包含头文件目录
target_include_directories(MpscQueueSimpleTest PRIVATE 
    ${MOODYCAMEL_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
# 设置编译定义
target_compile_definitions(MpscQueueSimpleTest PRIVATE 
    DEBUG 
    _DEBUG
)
set_target_properties(MpscQueueSimpleTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
# 添加pthread支持
target_link_libraries(MpscQueueSimpleTest PRIVATE pthread)
# 添加到测试集合
add_test(NAME MpscQueueSimpleTest COMMAND MpscQueueSimpleTest)

# ======================
# 创建测试集合目标
# ======================

# 创建 GPUBufferComponent 测试
add_executable(StandaloneRHI_GPUBufferComponent_Test 
    RHI/Components/TestGPUBufferComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(StandaloneRHI_GPUBufferComponent_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
add_test(NAME StandaloneRHI_GPUBufferComponent_Test COMMAND StandaloneRHI_GPUBufferComponent_Test)
set_target_properties(StandaloneRHI_GPUBufferComponent_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/RHI/Components"
)
target_compile_definitions(StandaloneRHI_GPUBufferComponent_Test PRIVATE DEBUG _DEBUG)


# 创建 TextureComponent 测试
add_executable(StandaloneRHI_TextureComponent_Test 
    RHI/Components/TestTextureComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/TextureComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(StandaloneRHI_TextureComponent_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
add_test(NAME StandaloneRHI_TextureComponent_Test COMMAND StandaloneRHI_TextureComponent_Test)
set_target_properties(StandaloneRHI_TextureComponent_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/RHI/Components" 
)
target_compile_definitions(StandaloneRHI_TextureComponent_Test PRIVATE DEBUG _DEBUG)

# 创建 RenderLayerComponent 测试
add_executable(StandaloneRHI_RenderLayerComponent_Test 
    RHI/Components/TestRenderLayerComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/RenderLayerComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(StandaloneRHI_RenderLayerComponent_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
add_test(NAME StandaloneRHI_RenderLayerComponent_Test COMMAND StandaloneRHI_RenderLayerComponent_Test)
set_target_properties(StandaloneRHI_RenderLayerComponent_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/RHI/Components"
)
target_compile_definitions(StandaloneRHI_RenderLayerComponent_Test PRIVATE DEBUG _DEBUG)

# 创建 MaterialComponent 测试
add_executable(StandaloneRHI_MaterialComponent_Test 
    RHI/Components/TestMaterialComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/MaterialComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Material.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/MaterialInstance.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(StandaloneRHI_MaterialComponent_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME StandaloneRHI_MaterialComponent_Test COMMAND StandaloneRHI_MaterialComponent_Test)
set_target_properties(StandaloneRHI_MaterialComponent_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/RHI/Components"
)
target_compile_definitions(StandaloneRHI_MaterialComponent_Test PRIVATE DEBUG _DEBUG)

# 创建RHI时间戳测试
add_executable(StandaloneRHI_Timestamp_Test RHI/Core/TestRHITimestamp.cpp)
target_include_directories(StandaloneRHI_Timestamp_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)
target_link_libraries(StandaloneRHI_Timestamp_Test PRIVATE Engine "-framework Foundation" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
add_test(NAME StandaloneRHI_Timestamp_Test COMMAND StandaloneRHI_Timestamp_Test)
set_target_properties(StandaloneRHI_Timestamp_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_Timestamp_Test PRIVATE DEBUG _DEBUG)

# 创建 RenderGraph Timestamp Integration 测试
add_executable(TestRenderGraphTimestamp 
    Graphics/RenderGraph/TestRenderGraphTimestamp.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderGraph/RenderGraphBuilder.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHISwapChain.cpp
)
target_include_directories(TestRenderGraphTimestamp PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC
)
target_link_libraries(TestRenderGraphTimestamp PRIVATE 
    Engine 
    "-framework Foundation" 
    "-framework Metal" 
    "-framework MetalKit" 
    "-framework QuartzCore"
)
add_test(NAME TestRenderGraphTimestamp COMMAND TestRenderGraphTimestamp)
set_target_properties(TestRenderGraphTimestamp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRenderGraphTimestamp PRIVATE DEBUG _DEBUG)
set_target_properties(TestRenderGraphTimestamp PROPERTIES FOLDER "Tests/Graphics")

# 创建RHI实体管理器测试
add_executable(RHI_EntityManager_Test 
    RHI/Core/TestRHIEntityManager.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIEntityManager.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/RenderLayerComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/MaterialComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Material.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/MaterialInstance.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
)
target_include_directories(RHI_EntityManager_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
# 链接Engine库 (或者不链接，只用源码，但如果依赖Engine其他部分可能需要链接)
# 这里尝试只包含源码，类似于其他测试
# 但如果RHIEntityManager依赖Engine的其他部分(如Memory)，可能需要链接Engine。
# 参考StandaloneRHI_MaterialComponent_Test，它没有链接Engine，只是包含了cpp文件。
# 但是 MaterialInstance.cpp 可能依赖其他东西。
# 让我们先试着像 MaterialComponent 测试那样包含 cpp 文件。
# 我需要包含所有相关的cpp文件。
# RHIEntityManager.cpp, RenderLayerComponent.cpp, MaterialComponent.cpp
# Material.cpp, MaterialInstance.cpp (because MaterialComponent uses them)
# RHIResource.cpp (Material uses handles)
# RHIGarbageCollector.cpp
add_test(NAME RHI_EntityManager_Test COMMAND RHI_EntityManager_Test)
set_target_properties(RHI_EntityManager_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/RHI/Core"
)
target_compile_definitions(RHI_EntityManager_Test PRIVATE DEBUG _DEBUG)

# 创建 RHI RenderSystem ECS Integration 测试
add_executable(TestRenderSystemECS 
    Graphics/TestRenderSystemECS.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Systems/RenderSystem.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIEntityManager.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Components/Pipeline.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/RenderLayerComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Components/MaterialComponent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/ForwardRenderer.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Passes/BlurPass.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Passes/SSRPass.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Utils/ShadowUtils.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderView.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderProxy.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RenderMesh.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Material.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/MaterialInstance.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIGarbageCollector.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHISwapChain.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(TestRenderSystemECS PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestRenderSystemECS COMMAND TestRenderSystemECS)
set_target_properties(TestRenderSystemECS PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/Graphics"
)
target_compile_definitions(TestRenderSystemECS PRIVATE DEBUG _DEBUG)

## 创建一个宏来运行所有RHI测试
add_custom_target(RHI_All_Tests
    COMMAND StandaloneRHI_Test
    COMMAND StandaloneRHI_Device_Test
    COMMAND StandaloneRHI_Resource_Test
    COMMAND StandaloneRHI_AdaptiveMemoryPool_Test
    COMMAND StandaloneRHI_Queue_Test
    COMMAND StandaloneRHI_DeterministicPrefetch_Test
    COMMAND StandaloneRHI_GPUOptimizer_Test
    COMMAND StandaloneRHI_MultiThreadedCommandGenerator_Test
    COMMAND StandaloneRHI_Allocator_Test
    COMMAND StandaloneRHI_GarbageCollector_Test
    COMMAND StandaloneRHI_Timestamp_Test
    COMMAND StandaloneRHI_GPUBufferComponent_Test
    COMMAND StandaloneRHI_RenderLayerComponent_Test
    COMMAND RHI_EntityManager_Test
    COMMAND TestRenderSystemECS
    COMMAND StandaloneGraphics_SceneDataAdapter_Test
    COMMAND MpscQueueSimpleTest
    COMMENT "Running all RHI tests"
)

# 为IDE创建文件夹结构
set_target_properties(StandaloneRHI_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Device_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Resource_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Queue_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_AdaptiveMemoryPool_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_DeterministicPrefetch_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_GPUOptimizer_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_MultiThreadedCommandGenerator_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Allocator_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_GarbageCollector_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_Timestamp_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(StandaloneRHI_GPUBufferComponent_Test PROPERTIES FOLDER "Tests/RHI")
set_target_properties(MpscQueueSimpleTest PROPERTIES FOLDER "Tests/RHI")
set_target_properties(RHI_All_Tests PROPERTIES FOLDER "Tests/RHI")

# 输出构建信息
message(STATUS "RHI Unit Tests configured:")
message(STATUS "  - StandaloneRHI_Test: Type system tests")
message(STATUS "  - StandaloneRHI_Device_Test: Device management tests") 
message(STATUS "  - StandaloneRHI_Resource_Test: Resource management tests")
message(STATUS "  - StandaloneRHI_Queue_Test: Command queue tests")
message(STATUS "  - StandaloneRHI_DeterministicPrefetch_Test: Deterministic prefetch tests")
message(STATUS "  - StandaloneRHI_GPUOptimizer_Test: GPU optimizer tests")
message(STATUS "  - StandaloneRHI_MultiThreadedCommandGenerator_Test: Multi-threaded command generator tests")
message(STATUS "  - StandaloneRHI_Allocator_Test: RHI Allocator tests")
message(STATUS "  - StandaloneRHI_GarbageCollector_Test: RHI Garbage Collector tests")
message(STATUS "  - StandaloneRHI_GPUBufferComponent_Test: GPU Buffer Component tests")
message(STATUS "  - StandaloneRHI_RenderLayerComponent_Test: Render Layer Component tests")
message(STATUS "  - RHI_All_Tests: Run all RHI tests")

# 创建RealTime PRT测试
add_executable(TestRealTimePRT Graphics/TestRealTimePRT.cpp)
target_include_directories(TestRealTimePRT PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestRealTimePRT COMMAND TestRealTimePRT)
set_target_properties(TestRealTimePRT PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRealTimePRT PRIVATE DEBUG _DEBUG)

# 创建 Light Probe 测试
add_executable(TestLightProbe 
    Graphics/TestLightProbe.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Lighting/LightProbeManager.cpp
)
target_include_directories(TestLightProbe PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
add_test(NAME TestLightProbe COMMAND TestLightProbe)
set_target_properties(TestLightProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestLightProbe PRIVATE DEBUG _DEBUG)

# 创建SceneDataAdapter测试
add_executable(StandaloneGraphics_SceneDataAdapter_Test 
    Graphics/TestSceneDataAdapter.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/SceneDataAdapter.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIResource.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHICommand.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Core/RHIDevice.cpp
)
target_include_directories(StandaloneGraphics_SceneDataAdapter_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(StandaloneGraphics_SceneDataAdapter_Test PRIVATE Engine)
add_test(NAME StandaloneGraphics_SceneDataAdapter_Test COMMAND StandaloneGraphics_SceneDataAdapter_Test)
set_target_properties(StandaloneGraphics_SceneDataAdapter_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneGraphics_SceneDataAdapter_Test PRIVATE DEBUG _DEBUG)

# ======================
# Particle System Tests
# ======================

# 创建粒子系统单元测试
add_executable(TestParticles 
    Particles/TestParticles.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Particles/ParticlePool.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Particles/ParticleSystem.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Particles/ParticleEmitter.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Particles/ParticleCPU.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Particles/ParticleCompute.cpp
)
target_include_directories(TestParticles PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestParticles PRIVATE Engine pthread)
add_test(NAME TestParticles COMMAND TestParticles)
set_target_properties(TestParticles PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "Tests/Particles"
)
target_compile_definitions(TestParticles PRIVATE DEBUG _DEBUG)

message(STATUS "Particle System Tests configured:")
message(STATUS "  - TestParticles: Particle pool and emitter unit tests")

# 创建RenderSystem集成测试
add_executable(TestRenderSystemIntegration Graphics/TestRenderSystemIntegration.cpp)
target_include_directories(TestRenderSystemIntegration PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestRenderSystemIntegration PRIVATE Engine)
add_test(NAME TestRenderSystemIntegration COMMAND TestRenderSystemIntegration)
set_target_properties(TestRenderSystemIntegration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRenderSystemIntegration PRIVATE DEBUG _DEBUG)
set_target_properties(TestRenderSystemIntegration PROPERTIES FOLDER "Tests/Graphics")


# ==================================
# Job System Tests
# ==================================

add_executable(TestJobSystem
    TestJobSystem.cpp
    ${CMAKE_SOURCE_DIR}/Engine/JobSystem/WorkStealingQueue.cpp
    ${CMAKE_SOURCE_DIR}/Engine/JobSystem/JobScheduler.cpp
    ${CMAKE_SOURCE_DIR}/Engine/JobSystem/JobSystem.cpp
)
target_include_directories(TestJobSystem PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestJobSystem COMMAND TestJobSystem)
set_target_properties(TestJobSystem PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestJobSystem PRIVATE DEBUG _DEBUG)
set_target_properties(TestJobSystem PROPERTIES FOLDER "Tests/JobSystem")

# JobSystem thread support
find_package(Threads REQUIRED)
target_link_libraries(TestJobSystem PRIVATE Threads::Threads)

# ==================================
# MaterialGraphAPI Tests
# ==================================

add_executable(TestMaterialGraphAPI TestMaterialGraphAPI.cpp)
target_include_directories(TestMaterialGraphAPI PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestMaterialGraphAPI PRIVATE EngineDLL)
add_test(NAME TestMaterialGraphAPI COMMAND TestMaterialGraphAPI)
set_target_properties(TestMaterialGraphAPI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestMaterialGraphAPI PRIVATE DEBUG _DEBUG)
set_target_properties(TestMaterialGraphAPI PROPERTIES FOLDER "Tests/MaterialGraph")

# ==================================
# MaterialGraph Reflection Tests (P0.2+P0.3+P0.4)
# ==================================

add_executable(TestMaterialReflection Graphics/TestMaterialReflection.cpp)
target_include_directories(TestMaterialReflection PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestMaterialReflection PRIVATE Engine)
add_test(NAME TestMaterialReflection COMMAND TestMaterialReflection)
set_target_properties(TestMaterialReflection PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestMaterialReflection PRIVATE DEBUG _DEBUG)
set_target_properties(TestMaterialReflection PROPERTIES FOLDER "Tests/MaterialGraph")

# ==================================
# ParticleMaterialAPI Tests (P2.8)
# ==================================

add_executable(TestParticleMaterialAPI Graphics/TestParticleMaterialAPI.cpp)
target_include_directories(TestParticleMaterialAPI PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestParticleMaterialAPI PRIVATE EngineDLL)
add_test(NAME TestParticleMaterialAPI COMMAND TestParticleMaterialAPI)
set_target_properties(TestParticleMaterialAPI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestParticleMaterialAPI PRIVATE DEBUG _DEBUG)
set_target_properties(TestParticleMaterialAPI PROPERTIES FOLDER "Tests/MaterialGraph")

# ==================================
# Material API Extended Tests (P1 Textures + P2.9 Batch)
# ==================================

add_executable(TestMaterialAPIExtended Graphics/TestMaterialAPIExtended.cpp)
target_include_directories(TestMaterialAPIExtended PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestMaterialAPIExtended PRIVATE EngineDLL)
add_test(NAME TestMaterialAPIExtended COMMAND TestMaterialAPIExtended)
set_target_properties(TestMaterialAPIExtended PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestMaterialAPIExtended PRIVATE DEBUG _DEBUG)
set_target_properties(TestMaterialAPIExtended PROPERTIES FOLDER "Tests/Material")

# ==================================
# ECS Light/Camera Component Tests
# ==================================

add_executable(TestLightComponent Components/TestLightComponent.cpp)
target_include_directories(TestLightComponent PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(TestLightComponent PRIVATE Engine)
add_test(NAME TestLightComponent COMMAND TestLightComponent)
set_target_properties(TestLightComponent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestLightComponent PRIVATE DEBUG _DEBUG)
set_target_properties(TestLightComponent PROPERTIES FOLDER "Tests/Components")

add_executable(TestCameraComponent Components/TestCameraComponent.cpp)
target_include_directories(TestCameraComponent PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(TestCameraComponent PRIVATE Engine)
add_test(NAME TestCameraComponent COMMAND TestCameraComponent)
set_target_properties(TestCameraComponent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestCameraComponent PRIVATE DEBUG _DEBUG)
set_target_properties(TestCameraComponent PROPERTIES FOLDER "Tests/Components")

# ==================================
# Phase 4.5: LightSyncSystem Integration Tests
# 验证 ECS Light + Transform → RenderScene::lights_ 闭环
# ==================================

add_executable(TestLightSyncSystem Components/TestLightSyncSystem.cpp)
target_include_directories(TestLightSyncSystem PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestLightSyncSystem PRIVATE Engine)
add_test(NAME TestLightSyncSystem COMMAND TestLightSyncSystem)
set_target_properties(TestLightSyncSystem PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestLightSyncSystem PRIVATE DEBUG _DEBUG)
set_target_properties(TestLightSyncSystem PROPERTIES FOLDER "Tests/Components")

# ==================================
# Phase 3: CameraSyncSystem Integration Tests
# 验证 ECS Camera + Transform → RenderView 矩阵闭环
# ==================================

add_executable(TestCameraSyncSystem Components/TestCameraSyncSystem.cpp)
target_include_directories(TestCameraSyncSystem PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestCameraSyncSystem PRIVATE Engine)
add_test(NAME TestCameraSyncSystem COMMAND TestCameraSyncSystem)
set_target_properties(TestCameraSyncSystem PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestCameraSyncSystem PRIVATE DEBUG _DEBUG)
set_target_properties(TestCameraSyncSystem PROPERTIES FOLDER "Tests/Components")

# ==================================
# Script System Foundation Tests
# 主线程检查 + initialize/shutdown 基础回归
# ==================================

add_executable(TestScriptFoundation Script/TestScriptFoundation.cpp)
target_include_directories(TestScriptFoundation PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptFoundation PRIVATE Engine)
add_test(NAME TestScriptFoundation COMMAND TestScriptFoundation)
set_target_properties(TestScriptFoundation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptFoundation PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptFoundation PROPERTIES FOLDER "Tests/Components")
# C++17 std::thread
find_package(Threads REQUIRED)
target_link_libraries(TestScriptFoundation PRIVATE Threads::Threads)

# ==================================
# Script System Lifecycle Tests (Phase 1 Task 2)
# fixed_update / late_update / destroy / on_reload hooks
# ==================================

add_executable(TestScriptLifecycle Script/TestScriptLifecycle.cpp)
target_include_directories(TestScriptLifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptLifecycle PRIVATE Engine)
add_test(NAME TestScriptLifecycle COMMAND TestScriptLifecycle)
set_target_properties(TestScriptLifecycle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptLifecycle PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptLifecycle PROPERTIES FOLDER "Tests/Components")

# ==================================
# Script System Property Reflection Tests (Phase 1 Task 4)
# entity_script::reflect(property_reflector&) — property_collector visitor
# ==================================

add_executable(TestScriptReflection Script/TestScriptReflection.cpp)
target_include_directories(TestScriptReflection PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptReflection PRIVATE Engine)
add_test(NAME TestScriptReflection COMMAND TestScriptReflection)
set_target_properties(TestScriptReflection PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptReflection PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptReflection PROPERTIES FOLDER "Tests/Components")

# ==================================
# Script System Event Bus Tests (Phase 1 Task 5)
# script_event_bus — type-erased subscribe/emit with FIFO queue,
# re-emit isolation (depth cap 8), deferred subscribe/unsubscribe
# ==================================

add_executable(TestScriptEventBus Script/TestScriptEventBus.cpp)
target_include_directories(TestScriptEventBus PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptEventBus PRIVATE Engine)
add_test(NAME TestScriptEventBus COMMAND TestScriptEventBus)
set_target_properties(TestScriptEventBus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptEventBus PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptEventBus PROPERTIES FOLDER "Tests/Components")
# Test uses typeid (RTTI) and std::function lambdas with captures that may throw
# on allocation failure; mirror Script.cpp per-file -fexceptions to be safe.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Script/TestScriptEventBus.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# ==================================
# Script System External C ABI Tests (Phase 1 Task 6)
# script_register_external / script_create_external — C ABI adapter,
# external_script lifecycle dispatch, C visitor reflect round-trip
# ==================================

add_executable(TestScriptExternal Script/TestScriptExternal.cpp)
target_include_directories(TestScriptExternal PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptExternal PRIVATE Engine)
add_test(NAME TestScriptExternal COMMAND TestScriptExternal)
set_target_properties(TestScriptExternal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptExternal PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptExternal PROPERTIES FOLDER "Tests/Components")
# Mirror Script.cpp per-file -fexceptions: test verifies that thrown exceptions
# from external callbacks are caught + logged (future test addition).
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Script/TestScriptExternal.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# ==================================
# Script System Hot Reload Tests (Phase 1 Task 7)
# external_script deferred reload: instance replacement, dedup,
# reload + remove same-frame precedence
# ==================================

add_executable(TestScriptReload Script/TestScriptReload.cpp)
target_include_directories(TestScriptReload PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptReload PRIVATE Engine)
add_test(NAME TestScriptReload COMMAND TestScriptReload)
set_target_properties(TestScriptReload PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptReload PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptReload PROPERTIES FOLDER "Tests/Components")
# Mirror Script.cpp per-file -fexceptions: reload dispatch wraps each hook
# in try/catch; test may exercise exception paths in future.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Script/TestScriptReload.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# ==================================
# Script System Event Bus External Tests (Phase 2b.2)
# script_event_subscribe/emit/unsubscribe/unsubscribe_all — string-keyed
# C ABI channel, opaque payload + deleter, parallel to templated bus
# ==================================

add_executable(TestScriptEventBusExternal Script/TestScriptEventBusExternal.cpp)
target_include_directories(TestScriptEventBusExternal PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptEventBusExternal PRIVATE Engine)
add_test(NAME TestScriptEventBusExternal COMMAND TestScriptEventBusExternal)
set_target_properties(TestScriptEventBusExternal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptEventBusExternal PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptEventBusExternal PROPERTIES FOLDER "Tests/Components")
# Mirror TestScriptEventBus.cpp per-file -fexceptions: tests verify that
# exceptions from handlers / deleters behave correctly.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Script/TestScriptEventBusExternal.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# ==================================
# Script System Post-Main-Thread MPSC Tests (Phase 1 Task 8)
# post_to_main_thread — lock-free MPSC queue for cross-thread callback submission
# ==================================

add_executable(TestScriptPostMainThread Script/TestScriptPostMainThread.cpp)
target_include_directories(TestScriptPostMainThread PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptPostMainThread PRIVATE Engine)
add_test(NAME TestScriptPostMainThread COMMAND TestScriptPostMainThread)
set_target_properties(TestScriptPostMainThread PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptPostMainThread PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptPostMainThread PROPERTIES FOLDER "Tests/Components")
# MPSC tests spawn std::thread — need Threads::Threads.
find_package(Threads REQUIRED)
target_link_libraries(TestScriptPostMainThread PRIVATE Threads::Threads)
# Mirror Script.cpp per-file -fexceptions: drain wraps each callback in
# try/catch; test may exercise exception paths in future.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Script/TestScriptPostMainThread.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# ==================================
# Phase 1 Task 9: Self-Test Backend (Pure C Integration Validation)
# Proves the C ABI in ScriptExternal.h can drive the script subsystem
# end-to-end from a pure-C source file (the same pattern Lua/Python/C#/mods
# will use). The .c file is compiled by the C compiler (CMake picks the
# compiler based on the .c extension), NOT the C++ frontend.
# ==================================

add_executable(TestScriptSelfTestBackend
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackend.c
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackendCpp.cpp
)
target_include_directories(TestScriptSelfTestBackend PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestScriptSelfTestBackend PRIVATE Engine)
add_test(NAME TestScriptSelfTestBackend COMMAND TestScriptSelfTestBackend)
set_target_properties(TestScriptSelfTestBackend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestScriptSelfTestBackend PRIVATE DEBUG _DEBUG)
set_target_properties(TestScriptSelfTestBackend PROPERTIES FOLDER "Tests/Components")
# Mirror Script.cpp per-file -fexceptions: the C++ driver's lambdas may
# allocate, and the adapter wraps each hook in try/catch.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackendCpp.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()
# Explicitly enable C99 for the .c file (Apple Clang defaults to gnu17 but
# this makes the intent visible and matches the // comment style we use).
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackend.c"
    PROPERTIES LANGUAGE C
)

# ==================================
# Phase 5: DebugDrawQueue Tests
# EngineDLL → Engine queue → ForwardSceneRenderer drain 的数据通道
# ==================================

add_executable(TestDebugDrawQueue Graphics/TestDebugDrawQueue.cpp)
target_include_directories(TestDebugDrawQueue PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestDebugDrawQueue PRIVATE Engine pthread)
add_test(NAME TestDebugDrawQueue COMMAND TestDebugDrawQueue)
set_target_properties(TestDebugDrawQueue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestDebugDrawQueue PRIVATE DEBUG _DEBUG)
set_target_properties(TestDebugDrawQueue PROPERTIES FOLDER "Tests/Graphics")

# ==================================
# WFC (Wave Function Collapse) Tests
# Phase A.1 foundation: WaveGrid Initialize/CellAt/Size
# ==================================

add_executable(TestWaveGrid Graphics/WFC/TestWaveGrid.cpp)
target_include_directories(TestWaveGrid PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWaveGrid PRIVATE Engine)
add_test(NAME TestWaveGrid COMMAND TestWaveGrid)
set_target_properties(TestWaveGrid PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWaveGrid PRIVATE DEBUG _DEBUG)
set_target_properties(TestWaveGrid PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestTileAdjacency Graphics/WFC/TestTileAdjacency.cpp)
target_include_directories(TestTileAdjacency PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestTileAdjacency PRIVATE Engine)
add_test(NAME TestTileAdjacency COMMAND TestTileAdjacency)
set_target_properties(TestTileAdjacency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestTileAdjacency PRIVATE DEBUG _DEBUG)
set_target_properties(TestTileAdjacency PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCConfig Graphics/WFC/TestWFCConfig.cpp)
target_include_directories(TestWFCConfig PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCConfig PRIVATE Engine)
add_test(NAME TestWFCConfig COMMAND TestWFCConfig)
set_target_properties(TestWFCConfig PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCConfig PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCConfig PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestAutoSocketClassifier Graphics/WFC/TestAutoSocketClassifier.cpp)
target_include_directories(TestAutoSocketClassifier PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestAutoSocketClassifier PRIVATE Engine)
add_test(NAME TestAutoSocketClassifier COMMAND TestAutoSocketClassifier)
set_target_properties(TestAutoSocketClassifier PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestAutoSocketClassifier PRIVATE DEBUG _DEBUG)
set_target_properties(TestAutoSocketClassifier PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestProceduralRoomPack Graphics/WFC/TestProceduralRoomPack.cpp)
target_include_directories(TestProceduralRoomPack PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestProceduralRoomPack PRIVATE Engine)
add_test(NAME TestProceduralRoomPack COMMAND TestProceduralRoomPack)
set_target_properties(TestProceduralRoomPack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestProceduralRoomPack PRIVATE DEBUG _DEBUG)
set_target_properties(TestProceduralRoomPack PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestTileAdjacencyBuild Graphics/WFC/TestTileAdjacencyBuild.cpp)
target_include_directories(TestTileAdjacencyBuild PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestTileAdjacencyBuild PRIVATE Engine)
add_test(NAME TestTileAdjacencyBuild COMMAND TestTileAdjacencyBuild)
set_target_properties(TestTileAdjacencyBuild PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestTileAdjacencyBuild PRIVATE DEBUG _DEBUG)
set_target_properties(TestTileAdjacencyBuild PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCSolveBudget Graphics/WFC/TestWFCSolveBudget.cpp)
target_include_directories(TestWFCSolveBudget PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCSolveBudget PRIVATE Engine)
add_test(NAME TestWFCSolveBudget COMMAND TestWFCSolveBudget)
set_target_properties(TestWFCSolveBudget PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCSolveBudget PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCSolveBudget PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCStepBuffer Graphics/WFC/TestWFCStepBuffer.cpp)
target_include_directories(TestWFCStepBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCStepBuffer PRIVATE Engine)
add_test(NAME TestWFCStepBuffer COMMAND TestWFCStepBuffer)
set_target_properties(TestWFCStepBuffer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCStepBuffer PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCStepBuffer PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCRandom Graphics/WFC/TestWFCRandom.cpp)
target_include_directories(TestWFCRandom PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCRandom PRIVATE Engine)
add_test(NAME TestWFCRandom COMMAND TestWFCRandom)
set_target_properties(TestWFCRandom PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCRandom PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCRandom PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCObserver Graphics/WFC/TestWFCObserver.cpp)
target_include_directories(TestWFCObserver PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCObserver PRIVATE Engine)
add_test(NAME TestWFCObserver COMMAND TestWFCObserver)
set_target_properties(TestWFCObserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCObserver PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCObserver PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCDistanceObserver Graphics/WFC/TestWFCDistanceObserver.cpp)
target_include_directories(TestWFCDistanceObserver PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCDistanceObserver PRIVATE Engine)
add_test(NAME TestWFCDistanceObserver COMMAND TestWFCDistanceObserver)
set_target_properties(TestWFCDistanceObserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCDistanceObserver PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCDistanceObserver PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCPropagator Graphics/WFC/TestWFCPropagator.cpp)
target_include_directories(TestWFCPropagator PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCPropagator PRIVATE Engine)
add_test(NAME TestWFCPropagator COMMAND TestWFCPropagator)
set_target_properties(TestWFCPropagator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCPropagator PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCPropagator PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestRestartPolicy Graphics/WFC/TestRestartPolicy.cpp)
target_include_directories(TestRestartPolicy PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestRestartPolicy PRIVATE Engine)
add_test(NAME TestRestartPolicy COMMAND TestRestartPolicy)
set_target_properties(TestRestartPolicy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestRestartPolicy PRIVATE DEBUG _DEBUG)
set_target_properties(TestRestartPolicy PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCTileRegistry Graphics/WFC/TestWFCTileRegistry.cpp)
target_include_directories(TestWFCTileRegistry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCTileRegistry PRIVATE Engine)
add_test(NAME TestWFCTileRegistry COMMAND TestWFCTileRegistry)
set_target_properties(TestWFCTileRegistry PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCTileRegistry PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCTileRegistry PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCSolver Graphics/WFC/TestWFCSolver.cpp)
target_include_directories(TestWFCSolver PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCSolver PRIVATE Engine)
add_test(NAME TestWFCSolver COMMAND TestWFCSolver)
set_target_properties(TestWFCSolver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCSolver PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCSolver PROPERTIES FOLDER "Tests/Graphics/WFC")

add_executable(TestWFCCategory
    Graphics/WFC/TestWFCCategory.cpp)
target_include_directories(TestWFCCategory PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests)
target_link_libraries(TestWFCCategory PRIVATE Engine)
add_test(NAME TestWFCCategory COMMAND TestWFCCategory)
set_target_properties(TestWFCCategory PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCCategory PRIVATE DEBUG _DEBUG)

add_executable(TestWFCTileLayout
    Graphics/WFC/TestWFCTileLayout.cpp)
target_include_directories(TestWFCTileLayout PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests)
target_link_libraries(TestWFCTileLayout PRIVATE Engine)
add_test(NAME TestWFCTileLayout COMMAND TestWFCTileLayout)
set_target_properties(TestWFCTileLayout PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCTileLayout PRIVATE DEBUG _DEBUG)

add_executable(TestWFCTileRegistryPacking
    Graphics/WFC/TestWFCTileRegistryPacking.cpp)
target_include_directories(TestWFCTileRegistryPacking PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCTileRegistryPacking PRIVATE Engine)
add_test(NAME TestWFCTileRegistryPacking COMMAND TestWFCTileRegistryPacking)
set_target_properties(TestWFCTileRegistryPacking PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCTileRegistryPacking PRIVATE DEBUG _DEBUG)

add_executable(TestWFCTileCatalog
    Graphics/WFC/TestWFCTileCatalog.cpp)
target_include_directories(TestWFCTileCatalog PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCTileCatalog PRIVATE Engine)
add_test(NAME TestWFCTileCatalog COMMAND TestWFCTileCatalog)
set_target_properties(TestWFCTileCatalog PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCTileCatalog PRIVATE DEBUG _DEBUG)

add_executable(TestParametricTileNode
    Graphics/WFC/TestParametricTileNode.cpp)
target_include_directories(TestParametricTileNode PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestParametricTileNode PRIVATE Engine)
add_test(NAME TestParametricTileNode COMMAND TestParametricTileNode)
set_target_properties(TestParametricTileNode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestParametricTileNode PRIVATE DEBUG _DEBUG)

add_executable(TestWFCOutput
    Graphics/WFC/TestWFCOutput.cpp)
target_include_directories(TestWFCOutput PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCOutput PRIVATE Engine)
add_test(NAME TestWFCOutput COMMAND TestWFCOutput)
set_target_properties(TestWFCOutput PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCOutput PRIVATE DEBUG _DEBUG)

# WFC Phase B.1 Task 5: TestWFCCornerMeshes — corner_in/out generator sanity
add_executable(TestWFCCornerMeshes
    Graphics/WFC/TestWFCCornerMeshes.cpp)
target_include_directories(TestWFCCornerMeshes PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCCornerMeshes PRIVATE Engine)
add_test(NAME TestWFCCornerMeshes COMMAND TestWFCCornerMeshes)
set_target_properties(TestWFCCornerMeshes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCCornerMeshes PRIVATE DEBUG _DEBUG)

# WFC Phase C.1 Task 6: TestWFCRuinsMaterialPalette — ruins palette sanity
add_executable(TestWFCRuinsMaterialPalette
    Graphics/WFC/TestWFCRuinsMaterialPalette.cpp)
target_include_directories(TestWFCRuinsMaterialPalette PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCRuinsMaterialPalette PRIVATE Engine)
add_test(NAME TestWFCRuinsMaterialPalette COMMAND TestWFCRuinsMaterialPalette)
set_target_properties(TestWFCRuinsMaterialPalette PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCRuinsMaterialPalette PRIVATE DEBUG _DEBUG)

# WFC Phase C.1 Task 7: TestWFCProceduralMeshesRuins — RegisterProceduralMesh signature
add_executable(TestWFCProceduralMeshesRuins
    Graphics/WFC/TestWFCProceduralMeshesRuins.cpp)
target_include_directories(TestWFCProceduralMeshesRuins PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCProceduralMeshesRuins PRIVATE Engine)
add_test(NAME TestWFCProceduralMeshesRuins COMMAND TestWFCProceduralMeshesRuins)
set_target_properties(TestWFCProceduralMeshesRuins PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCProceduralMeshesRuins PRIVATE DEBUG _DEBUG)

# WFC Phase B.2 Task 1: TestWFCStreamDrain — DrainStream unit tests
add_executable(TestWFCStreamDrain
    Graphics/WFC/TestWFCStreamDrain.cpp)
target_include_directories(TestWFCStreamDrain PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCStreamDrain PRIVATE Engine)
add_test(NAME TestWFCStreamDrain COMMAND TestWFCStreamDrain)
set_target_properties(TestWFCStreamDrain PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCStreamDrain PRIVATE DEBUG _DEBUG)

# WFC Phase C.1 Task 13: TestWFCSocketOps — GetFaceCorners + QuantizeTo2Bit helpers
add_executable(TestWFCSocketOps
    Graphics/WFC/TestWFCSocketOps.cpp)
target_include_directories(TestWFCSocketOps PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCSocketOps PRIVATE Engine)
add_test(NAME TestWFCSocketOps COMMAND TestWFCSocketOps)
set_target_properties(TestWFCSocketOps PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCSocketOps PRIVATE DEBUG _DEBUG)

# WFC Phase C.1 Mixed Task 1: TestWFCCandidateLayout — u64[4] mask layout + MaxTiles bump
# NOTE: Header-only canary — verifies WFCTypes/WFCTileRegistry constexpr layout
# (kMaskWords, MaxTiles, BitForTileVariant). No Engine link needed because the
# test never constructs a WFCTileRegistry instance. Tasks 2-3 do not need to
# revisit this entry; the bypass is intentional and permanent for this file.
add_executable(TestWFCCandidateLayout
    Graphics/WFC/TestWFCCandidateLayout.cpp)
target_include_directories(TestWFCCandidateLayout PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
add_test(NAME TestWFCCandidateLayout COMMAND TestWFCCandidateLayout)
set_target_properties(TestWFCCandidateLayout PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    FOLDER "EngineTest/UnitTests")
target_compile_definitions(TestWFCCandidateLayout PRIVATE DEBUG _DEBUG)

