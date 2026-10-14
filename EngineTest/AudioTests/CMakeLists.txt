# AudioTests — audio S2 空间声/混音单测（纯数学，无设备依赖）。
add_executable(TestAudioSpatial TestAudioSpatial.cpp)
target_include_directories(TestAudioSpatial PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Engine ${CMAKE_SOURCE_DIR}/Engine/Common)
target_link_libraries(TestAudioSpatial PRIVATE Engine)
add_test(NAME UnitTestAudioSpatial COMMAND TestAudioSpatial)
