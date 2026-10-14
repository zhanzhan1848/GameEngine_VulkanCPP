#pragma once

// NavLua.h — ai-navigation S1 收口（2026-10-15）：Lua `nav` 表注册器。
//   nav.move_to(x, y, z [, speed]) → bool   为当前实体（全局 entity_id）建/换任务
//   nav.stop()                              取消当前实体任务
//   nav.state() → string                    "idle"/"moving"/"completed"/"cancelled"/"failed:<code>"
// 与 InputLua.h 同层同构（Engine/Scripting，EngineDLL 的 extra_globals_registrar
// 与测试共用单份实现）。entity_id 全局由 LuaBackend 每实例设置。

// third_party/lua 的头不带 extern "C" 自守卫（ScriptAPI.cpp 同款手动包裹）。
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include "Navigation/NavigationRuntime.h"

namespace primal::scripting {

inline int lua_nav_move_to(lua_State* L)
{
    const f32 x = (f32)luaL_checknumber(L, 1);
    const f32 y = (f32)luaL_checknumber(L, 2);
    const f32 z = (f32)luaL_checknumber(L, 3);
    const f32 speed = lua_gettop(L) >= 4 ? (f32)lua_tonumber(L, 4) : 0.f;
    lua_getglobal(L, "entity_id");
    const u64 entity = (u64)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (entity == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const auto err = navigation::runtime::move_to(entity, math::v3{ x, y, z }, speed);
    lua_pushboolean(L, err == navigation::nav_error::ok ? 1 : 0);
    return 1;
}

inline int lua_nav_stop(lua_State* L)
{
    lua_getglobal(L, "entity_id");
    const u64 entity = (u64)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (entity != 0) navigation::runtime::stop(entity);
    return 0;
}

inline int lua_nav_state(lua_State* L)
{
    lua_getglobal(L, "entity_id");
    const u64 entity = (u64)lua_tonumber(L, -1);
    lua_pop(L, 1);
    using navigation::move_state;
    using navigation::nav_error;
    const move_state s = entity != 0 ? navigation::runtime::state_of(entity) : move_state::idle;
    const char* name = "idle";
    switch (s) {
        case move_state::idle:     name = "idle"; break;
        case move_state::moving:   name = "moving"; break;
        case move_state::completed:name = "completed"; break;
        case move_state::cancelled:name = "cancelled"; break;
        case move_state::failed:
            name = entity != 0
                ? navigation::to_string(navigation::runtime::failure_of(entity))
                : "failed";
            break;
    }
    if (s == move_state::failed) {
        char buf[48];
        snprintf(buf, sizeof(buf), "failed:%s", name);
        lua_pushstring(L, buf);
    } else {
        lua_pushstring(L, name);
    }
    return 1;
}

inline void register_nav_table(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, &lua_nav_move_to); lua_setfield(L, -2, "move_to");
    lua_pushcfunction(L, &lua_nav_stop);    lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, &lua_nav_state);   lua_setfield(L, -2, "state");
    lua_setglobal(L, "nav");
}

} // namespace primal::scripting
