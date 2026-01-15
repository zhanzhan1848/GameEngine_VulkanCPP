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
add_executable(StandaloneRHI_ShadowUtils_Test Graphics/TestShadowUtils.cpp)
target_include_directories(StandaloneRHI_ShadowUtils_Test PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)
target_link_libraries(StandaloneRHI_ShadowUtils_Test PRIVATE Engine)
add_test(NAME StandaloneRHI_ShadowUtils_Test COMMAND StandaloneRHI_ShadowUtils_Test)
set_target_properties(StandaloneRHI_ShadowUtils_Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(StandaloneRHI_ShadowUtils_Test PRIVATE DEBUG _DEBUG)


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

    # 创建Metal Shader 熱重载测试
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
    # 不链接Engine库，使用源码编译以确保符号可见
    # target_link_libraries(StandaloneGraphics_Material_Test PRIVATE Engine)
    add_test(NAME StandaloneGraphics_Material_Test COMMAND StandaloneGraphics_Material_Test)
    set_target_properties(StandaloneGraphics_Material_Test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
    )
    target_compile_definitions(StandaloneGraphics_Material_Test PRIVATE DEBUG _DEBUG)
    set_target_properties(StandaloneGraphics_Material_Test PROPERTIES FOLDER "Tests/Graphics")
    
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
message(STATUS "  - RHI_All_Tests: Run all RHI tests")
