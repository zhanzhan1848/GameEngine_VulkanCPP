# PrimalPlayer — 运行时宿主（规划 §Run/Stop，2026-09-03）
# 编辑器 Run 按钮的启动目标：读运行时场景包（primal-runtime-scene v1）→
# EngineDLL C ABI 注入 → 窗口 + 渲染循环。C ABI 消费者纪律与 PrimalEngineCli
# 相同：只依赖导出面，链接期 dyld 校验符号。
# 分图形API/分 debug/release = 各构建树（Darwin/Debug = Metal、
# Darwin/DebugVulkan = Vulkan、Darwin/Release）各自产出一份本宿主。
set(PROJECT_NAME PrimalPlayer)

cmake_minimum_required(VERSION 3.20)

if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

add_executable(${PROJECT_NAME} "main.cpp" "player_window.mm")

if(APPLE)
    # .mm 走 C++ 编译器 + per-source `-x objective-c++`（enable_language(OBJCXX)
    # 在子目录启用太晚，缺 CMAKE_OBJCXX_COMPILE_OBJECT）。
    set_source_files_properties("player_window.mm" PROPERTIES
        LANGUAGE CXX
        COMPILE_FLAGS "-x objective-c++")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineDLL
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        "PRIMAL_PLUS"
        "USE_WITH_EDITOR"
        "_HAS_EXCEPTIONS=0"
        "_WINDOWS"
        "UNICODE"
        "_UNICODE"
    )
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /GR /EHs-c- /Y- /MP)
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PRIMAL_PLUS" "USE_WITH_EDITOR")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -fno-exceptions)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework AVFoundation")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PRIMAL_PLUS" "USE_WITH_EDITOR")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -fno-exceptions -fPIC)
    target_link_libraries(${PROJECT_NAME} PRIVATE dl pthread)
endif()

# C ABI 消费者：只依赖 EngineDLL 的导出面（与编辑器/primal-engine 平级）。
target_link_libraries(${PROJECT_NAME} PRIVATE EngineDLL)

# 运行时依赖部署：dylib 与宿主同目录（引擎 shader blob 亦按 cwd=exe 目录解析，
# main() 自己 chdir 到 exe 目录）。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:EngineDLL>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Deploying EngineDLL next to PrimalPlayer"
)

# R9: Agility runtime DLL staging (.\D3D12\ next to this exe; no-op outside
# Windows+D3D12 trees).
primal_stage_d3d12_agility(${PROJECT_NAME})
