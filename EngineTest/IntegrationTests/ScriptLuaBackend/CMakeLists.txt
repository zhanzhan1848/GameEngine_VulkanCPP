# Lua 5.4 static library built from third_party/lua/ sources.
# Compiled without the standalone interpreter (lua.c) and compiler binary (luac.c).
set(LUA_SRC
    ${CMAKE_SOURCE_DIR}/third_party/lua/lapi.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lcode.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lctype.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ldebug.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ldo.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ldump.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lfunc.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lgc.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/llex.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lmem.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lobject.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lopcodes.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lparser.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lstate.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lstring.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ltable.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ltm.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lundump.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lvm.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lzio.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lauxlib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lbaselib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lcorolib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ldblib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/liolib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/linit.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lmathlib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/loadlib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/loslib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lstrlib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/ltablib.c
    ${CMAKE_SOURCE_DIR}/third_party/lua/lutf8lib.c
)

add_library(lua_static STATIC ${LUA_SRC})
target_include_directories(lua_static PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua)

# Lua 5.4 needs a platform define for default config
# （Windows 下必须用 LUA_USE_WINDOWS，POSIX 宏会引入 sys/wait.h、dlfcn.h）
if(APPLE)
    target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX=1)
elseif(WIN32)
    target_compile_definitions(lua_static PRIVATE LUA_USE_WINDOWS=1)
else()
    target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX=1)
endif()

# === Phase 2a Task 2: TestLuaBackend skeleton ===

add_executable(TestLuaBackend
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptLuaBackend/TestLuaBackend.cpp
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptLuaBackend/LuaBackend.cpp
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackend.c
)
target_include_directories(TestLuaBackend PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
    ${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptLuaBackend
)
target_link_libraries(TestLuaBackend PRIVATE Engine lua_static)
add_test(NAME TestLuaBackend COMMAND TestLuaBackend)
set_target_properties(TestLuaBackend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestLuaBackend PRIVATE $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)
set_target_properties(TestLuaBackend PROPERTIES FOLDER "Tests/Components")

# Script.cpp uses -fexceptions; the Lua adapter wraps each hook in try/catch
# and the test driver's lambdas may allocate.
if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptLuaBackend/LuaBackend.cpp"
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

# Force ScriptSelfTestBackend.c to compile as C (not C++). CMake's language
# detection should handle this, but we make it explicit to match the separate
# TestScriptSelfTestBackend target and guarantee C ABI fidelity.
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/EngineTest/IntegrationTests/ScriptSelfTestBackend.c"
    PROPERTIES LANGUAGE C
)
