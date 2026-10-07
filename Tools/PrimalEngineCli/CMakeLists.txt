# primal-engine — 离线通道 one-shot CLI（CLI 规划 §3.1/§10 Phase 2）
# 可沙箱、无 GUI、无 dotnet 依赖；与 libEngineDLL 同目录输出以便加载。
set(PROJECT_NAME PrimalEngineCli)

cmake_minimum_required(VERSION 3.20)

if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

add_executable(${PROJECT_NAME} "main.cpp")

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "primal-engine"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_SYSTEM_NAME}/$<CONFIG>/"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineDLL
    # A9 PR2: AIAssetPipelineParams.h（权威 POD 头）+ pipeline/SceneBlobReader.h
    # （cook 产物读回校验）。
    ${CMAKE_SOURCE_DIR}/ContentTools
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
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PRIMAL_PLUS" "USE_WITH_EDITOR")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -fno-exceptions -fPIC)
    target_link_libraries(${PROJECT_NAME} PRIVATE dl pthread)
endif()

# C ABI 消费者：只依赖 EngineDLL 的导出面（与编辑器/脚本宿主平级，规划 §12.1）。
# ContentTools 提供 cook-mesh 等资产离线 verb；contenttools_pipeline 提供
# A9 PR2 的产物读回（SceneBlobReader WalkSceneBlob）。
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE EngineDLL ContentTools contenttools_pipeline ZLIB::ZLIB)

# Offline command surface has its own snapshot (the editor attribute registry
# snapshot does not include these one-shot verbs). No GPU or running editor.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME PrimalEngineCliSchema
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Testing/CliSchema/test_engine_schema.py
                --engine $<TARGET_FILE:${PROJECT_NAME}>)
endif()

# 运行时依赖部署：本 exe 的输出目录与两个 DLL 的输出目录在 Windows 的多配置
# 生成器下不同目录（EngineDLL→Windows/<cfg>，ContentTools→build/bin/<cfg>），
# 不拷贝就无法加载。macOS/Linux 天然同目录，copy_if_different 为 no-op。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:EngineDLL>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ContentTools>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Deploying EngineDLL/ContentTools next to primal-engine"
)
