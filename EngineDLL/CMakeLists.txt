# 设置项目名称
set(PROJECT_NAME EngineDLL)

# CMake最低版本要求
cmake_minimum_required(VERSION 3.20)

# 源文件和头文件
set(SOURCE_FILES
    "DllMain.cpp"
    "EngineAPI.cpp"
    "EntityAPI.cpp"
    "AssetAPI.cpp"
    "RenderPipelineAPI.cpp"
    "PCGAPI.cpp"
    "GeometryAPI.cpp"
    "MaterialAPI.cpp"
    "MaterialPreviewAPI.cpp"
    "MaterialGraphAPI.cpp"
    "MaterialGraphReflectionAPI.cpp"
    "ParticleMaterialAPI.cpp"
    "ShaderCompilerAPI.cpp"
    "FrameAPI.cpp"
    "ResourceAPI.cpp"
    "DebugDrawAPI.cpp"
    "CaptureAPI.cpp"
    "RenderTargetAPI.cpp"
    "LightAPI.cpp"
    "CameraAPI.cpp"
    "ProceduralMeshAPI.cpp"
)

set(HEADER_FILES
    "Common.h"
    "EngineAPIInternal.h"
    "FrameAPI.h"
    "PipelineLightDesc.h"
)

# 添加共享库目标
add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES} ${HEADER_FILES})

# 设置C++标准
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# 设置输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
)

# 包含目录
target_include_directories(${PROJECT_NAME} PUBLIC
    # "${CMAKE_CURRENT_SOURCE_DIR}/../Engine"
    # "${CMAKE_CURRENT_SOURCE_DIR}/../Engine/Common"
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/Engine/Content
    ${CMAKE_SOURCE_DIR}/Content
)

# 平台特定设置
if(WIN32)
    # Windows特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "USE_WITH_EDITOR"
        "ENGINEDLL_EXPORTS"
        "_WINDOWS"
        "_USRDLL"
        "_HAS_EXCEPTIONS=0"
        "UNICODE"
        "_UNICODE"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:DebugEditor>:DEBUG>"
        "$<$<CONFIG:Release>:DEBUG>"
        "$<$<CONFIG:ReleaseEditor>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /MP
        /GR             # 与 Engine 一致：RTTI 开（/GR- 下 dynamic_cast 运行时抛
                        # bad_cast，异常已禁 → 0xc0000409）
        /fp:fast         # 快速浮点运算
        /EHs-c-         # 禁用异常处理
        /Y-             # 禁用预编译头
        $<$<CONFIG:Debug>:/Od /ZI>
        $<$<CONFIG:DebugEditor>:/Od /ZI>
        $<$<CONFIG:Release>:/Od /ZI>
        $<$<CONFIG:ReleaseEditor>:/Ox /Oi /Oy /Gy ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT}>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        /SUBSYSTEM:WINDOWS
        /ignore:4099
        $<$<CONFIG:Debug>:/DEBUG /INCREMENTAL>
        $<$<CONFIG:DebugEditor>:/DEBUG /INCREMENTAL>
        $<$<CONFIG:Release>:/DEBUG /INCREMENTAL>
        $<$<CONFIG:ReleaseEditor>:/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
elseif(APPLE)
    # macOS特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "USE_WITH_EDITOR"
        "_HAS_EXCEPTIONS=0"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        # -fno-rtti              # 禁用RTTI
        -ffast-math            # 快速浮点运算
        -fno-exceptions        # 禁用异常处理
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3>
    )

    # macOS特定的框架
    # target_link_libraries(${PROJECT_NAME} PRIVATE
    #     "-framework Cocoa"
    #     "-framework Metal"
    #     "-framework QuartzCore"
    #     "-framework AppKit"
        
    # )
else()
    # Linux特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "USE_WITH_EDITOR"
        "_HAS_EXCEPTIONS=0"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        # -fno-rtti              # 禁用RTTI
        -ffast-math            # 快速浮点运算
        -fno-exceptions        # 禁用异常处理
        -fPIC                  # 位置无关代码
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3>
    )

    # Linux特定的库
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dl
        pthread
        X11
        vulkan
    )
endif()

# 链接依赖项
target_link_libraries(${PROJECT_NAME} PRIVATE
    Engine
    # ContentTools
)

# EngineDLL.dll 部署到测试输出目录：TestMaterialGraphAPI / TestParticleMaterialAPI /
# TestMaterialAPIExtended 直接链接本 DLL。此前 DLL 只输出到源码树 Windows/，exe
# 启动时找不到 → STATUS_DLL_NOT_FOUND(0xc0000135)。VS 生成器把 exe 放在
# Tests/UnitTests/$<CONFIG>/（自动追加配置子目录），Ninja/Makefile 直接放
# Tests/UnitTests/——两个布局各拷一份（make_directory 保证目录存在）。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_BINARY_DIR}/Tests/UnitTests/$<CONFIG>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            ${CMAKE_BINARY_DIR}/Tests/UnitTests/$<CONFIG>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            ${CMAKE_BINARY_DIR}/Tests/UnitTests/
    COMMENT "Deploying EngineDLL to Tests/UnitTests (config + root layouts)"
)

# 配置编译器特性
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

# 如果是MSVC，设置运行时库
if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()
