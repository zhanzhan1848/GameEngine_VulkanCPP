#pragma once

// GameLua.h — 玩法规则骨架 L2：Lua `game` 表受控写入口（gameplay-rules.md）。
//   game.state.get()                → string|nil   当前实体状态机的当前状态
//   game.state.transition("to")     → bool, string? err   受控转移（结构校验：
//                                     (当前态→to) 边必须存在于转移表；guard 只
//                                     约束自动路径，Lua 是决策者）
//   game.task.complete("id") (S2)
//   game.task.fail("id")      (S2)
// 契约写入口收窄于此：脚本不得绕过状态机直改契约数据。与 NavLua.h 同层
// 同构；entity_id 全局由 LuaBackend 每实例设置。

// third_party/lua 的头不带 extern "C" 自守卫（ScriptAPI.cpp 同款手动包裹）。
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include "Gameplay/GameplayRuntime.h"

#include <string>

namespace primal::scripting {

inline u64 lua_self_entity(lua_State* L)
{
    lua_getglobal(L, "entity_id");
    const u64 entity = (u64)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return entity;
}

inline int lua_game_state_get(lua_State* L)
{
    const u64 entity = lua_self_entity(L);
    std::string state;
    if (entity == 0 || !gameplay::runtime::state_get(entity, state)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, state.c_str());
    return 1;
}

inline int lua_game_state_transition(lua_State* L)
{
    const char* to = luaL_checkstring(L, 1);
    const u64 entity = lua_self_entity(L);
    std::string detail;
    const gameplay::gp_error err =
        entity == 0 ? gameplay::gp_error::no_machine
                    : gameplay::runtime::state_transition(entity, to, detail);
    if (err == gameplay::gp_error::ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    if (detail.empty()) detail = gameplay::to_string(err);
    lua_pushstring(L, detail.c_str());
    return 2;
}

// game.task.complete / game.task.fail 由 S2 任务契约接入（同文件追加）。

inline void register_game_table(lua_State* L)
{
    lua_newtable(L);                                        // game
    lua_newtable(L);                                        // game.state
    lua_pushcfunction(L, &lua_game_state_get);       lua_setfield(L, -2, "get");
    lua_pushcfunction(L, &lua_game_state_transition);lua_setfield(L, -2, "transition");
    lua_setfield(L, -2, "state");
    lua_setglobal(L, "game");
}

}  // namespace primal::scripting
