# PhysicsTests — physics S2 铰链关节单测（无 GPU 依赖）。
add_executable(TestPhysicsJoints TestPhysicsJoints.cpp)
target_include_directories(TestPhysicsJoints PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Engine ${CMAKE_SOURCE_DIR}/Engine/Common)
target_link_libraries(TestPhysicsJoints PRIVATE Engine)
add_test(NAME UnitTestPhysicsJoints COMMAND TestPhysicsJoints)
