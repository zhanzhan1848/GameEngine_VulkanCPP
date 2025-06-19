# 设置项目名称
set(PROJECT_NAME EngineTest)

# CMake最低版本要求
cmake_minimum_required(VERSION 3.20)

# 设置编译器标志
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++17")
    
# 设置Objective-C++标志
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -std=c++17")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS")

# 源文件列表
set(SOURCE_FILES
    # "Lights.cpp"
    "Main.cpp"
    "RenderItem.cpp"
    "Scripts.cpp"
    "ShaderCompilation.cpp"
    # "TestRenderer.cpp"
    "TestRendererMac.cpp"
    "MacKeyboard.mm"
)

# 头文件列表
set(HEADER_FILES
    "PythonScript.h"
    "ShaderCompilation.h"
    "Test.h"
    "TestEntityComponents.h"
    "TestRenderer.h"
    "TestWindow.h"
    "TestWindowLinux.h"
    "MacKeyboard.h"
)

# 添加可执行文件目标
add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${HEADER_FILES})

# 设置C++标准
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# 设置输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
)

# 包含目录
target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../Engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Engine/Common"
    ${CMAKE_SOURCE_DIR}/OSAPI/MAC/
)

# 平台特定设置
if(WIN32)
    # Windows特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "_CONSOLE"
        "_HAS_EXCEPTIONS=0"
        "UNICODE"
        "_UNICODE"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /MP
        /GR-             # 禁用RTTI
        /fp:fast         # 快速浮点运算
        /EHs-c-         # 禁用异常处理
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        /SUBSYSTEM:WINDOWS
        /IGNORE:4099
        $<$<CONFIG:Debug>:/DEBUG>
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
    )
elseif(APPLE)
    # macOS特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -fno-rtti              # 禁用RTTI
        -ffast-math            # 快速浮点运算
        # -fno-exceptions        # 禁用异常处理
        -pthread
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3>
    )

    # macOS特定的框架
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AppKit"
    )
else()
    # Linux特定的编译选项和定义
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "$<$<CONFIG:Debug>:DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
    )
    
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -fno-rtti              # 禁用RTTI
        -ffast-math            # 快速浮点运算
        -fno-exceptions        # 禁用异常处理
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3>
    )

    # Linux特定的库
    target_link_libraries(${PROJECT_NAME} PRIVATE
        X11
        vulkan
    )
endif()

# 链接依赖项
target_link_libraries(${PROJECT_NAME} PRIVATE
    Engine
    ContentTools
)

# 配置编译器特性
if(MSVC)
    target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
else()
    target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
    target_compile_options(${PROJECT_NAME} PRIVATE -fPIC)
endif()

# 复制着色器和资源文件
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
)
