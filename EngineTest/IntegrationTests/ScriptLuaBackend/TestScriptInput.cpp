// input-camera S1（2026-10-14）：InputState + Lua input 表契约测试。
// 纯 Lua 状态机级验证——不经 LuaBackend/引擎脚本子系统，直接在裸 lua_State
// 上注册表并执行断言脚本；宿主写入（InputState::set）与脚本读取
//（input.is_down）的双向语义在这里锁定。
#include "Scripting/InputState.h"
#include "Scripting/InputLua.h"

#include <cassert>
#include <cstdio>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
// InputLua.h 已含 lua.h（extern "C" 包裹）；这里显式补 lauxlib（luaL_newstate/dostring）。

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::printf("PASS: %s\n", what.c_str());
    }
}

// 在 L 上执行 expr 并要求返回布尔 expected。
bool lua_bool_expr(lua_State* L, const char* expr, bool expected)
{
    const std::string chunk = "return " + std::string(expr);
    if (luaL_dostring(L, chunk.c_str()) != LUA_OK) {
        std::fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    const bool got = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return got == expected;
}

} // namespace

int main()
{
    // --- 纯状态机语义 ---
    auto& input = primal::scripting::InputState::instance();
    input.clear();
    check(!input.is_down("MoveForward"), "未注册动作 = false");
    input.set("MoveForward", true);
    check(input.is_down("MoveForward"), "写入 down 后可读");
    check(input.registered_count() == 1, "注册计数");
    input.set("MoveForward", false);
    check(!input.is_down("MoveForward"), "写入 up 覆盖 down");
    input.set("Jump", true);
    input.clear();
    check(!input.is_down("Jump") && input.registered_count() == 0,
          "clear 清空全部动作");
    // 大小写敏感（映射双方用同一字符串是既定契约）
    input.set("Jump", true);
    check(!input.is_down("jump"), "动作名区分大小写");

    // --- Lua input 表（裸 lua_State + 共用注册器）---
    lua_State* L = luaL_newstate();
    assert(L);
    primal::scripting::register_input_table(L);

    check(lua_bool_expr(L, "input.is_down('Jump')", true), "Lua 读取 down 动作");
    check(lua_bool_expr(L, "input.is_down('MoveForward')", false), "Lua 读取 up 动作");
    check(lua_bool_expr(L, "input.is_down('NoSuchAction')", false),
          "Lua 未注册动作 = false");
    check(lua_bool_expr(L, "input.is_down()", false), "Lua 缺参不崩（nil→false）");

    input.set("MoveForward", true);
    check(lua_bool_expr(L, "input.is_down('MoveForward')", true),
          "宿主帧间更新对脚本即时可见");

    lua_close(L);
    input.clear();

    if (g_failures == 0) std::printf("All script input tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
