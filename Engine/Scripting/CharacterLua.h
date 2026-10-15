#pragma once

// CharacterLua.h — physics S2 收尾（2026-10-16）：Lua `character` 表注册器。
//   character.move(x, z [, speed?]) → bool   为当前实体（全局 entity_id）设
//                                            移动意图（世界空间 XZ，自动归一）
//   character.stop() → bool                  零意图（摩擦停止）
//   character.state() → string               "moving"/"stuck"/"grounded"/"air"/"idle"
// 与 InputLua/NavLua/AudioLua 同构（函数指针桥，EngineDLL ScriptAPI 注入实现）。

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace primal::scripting {

struct character_api_bridge
{
    unsigned (*move)(unsigned long long entity, float x, float z);
    unsigned (*state)(unsigned long long entity, float* out8);   // out8 见 PhysicsAPI 注释
};

inline character_api_bridge g_character_bridge{};

inline unsigned long long lua_current_entity(lua_State* L)
{
    lua_getglobal(L, "entity_id");
    const unsigned long long e = (unsigned long long)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return e;
}

inline int lua_character_move(lua_State* L)
{
    const float x = (float)luaL_checknumber(L, 1);
    const float z = (float)luaL_checknumber(L, 2);
    const auto entity = lua_current_entity(L);
    if (entity == 0 || !g_character_bridge.move)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_character_bridge.move(entity, x, z) ? 1 : 0);
    return 1;
}

inline int lua_character_stop(lua_State* L)
{
    const auto entity = lua_current_entity(L);
    if (entity == 0 || !g_character_bridge.move)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_character_bridge.move(entity, 0.f, 0.f) ? 1 : 0);
    return 1;
}

inline int lua_character_state(lua_State* L)
{
    const auto entity = lua_current_entity(L);
    float st[8] = { 0 };
    if (entity == 0 || !g_character_bridge.state || !g_character_bridge.state(entity, st))
    {
        lua_pushstring(L, "none");
        return 1;
    }
    const bool grounded = st[3] != 0.f;
    const bool stuck = st[5] != 0.f;
    const float speed = st[4];
    const float ix = st[6], iz = st[7];
    const bool hasIntent = ix != 0.f || iz != 0.f;
    const char* name = "idle";
    if (stuck) name = "stuck";
    else if (!grounded) name = "air";
    else if (hasIntent && speed > 0.1f) name = "moving";
    else if (hasIntent) name = "moving";   // 意图已设速度尚低（起步）
    lua_pushstring(L, name);
    return 1;
}

inline void register_character_table(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, &lua_character_move);  lua_setfield(L, -2, "move");
    lua_pushcfunction(L, &lua_character_stop);  lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, &lua_character_state); lua_setfield(L, -2, "state");
    lua_setglobal(L, "character");
}

} // namespace primal::scripting
