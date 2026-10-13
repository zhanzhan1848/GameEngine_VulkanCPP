#pragma once

// input-camera S1（2026-10-14）：Lua `input` 表注册器。
// `input.is_down("<动作名>")` → bool；未注册动作恒 false。
// 与 InputState.h 同层（Engine/Scripting），供 EngineDLL 的
// extra_globals_registrar 与测试直接共用——注册逻辑只有一份实现。

// third_party/lua 的头不带 extern "C" 自守卫（ScriptAPI.cpp 同款手动包裹）。
extern "C" {
#include "lua.h"
}
#include "InputState.h"

namespace primal::scripting {

inline int lua_input_is_down(lua_State* L)
{
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, (name && InputState::instance().is_down(name)) ? 1 : 0);
    return 1;
}

inline void register_input_table(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, &lua_input_is_down);
    lua_setfield(L, -2, "is_down");
    lua_setglobal(L, "input");
}

} // namespace primal::scripting
