#pragma once

// AudioLua.h — audio S2（2026-10-15）：Lua `audio` 表注册器。
//   audio.play(path [, group] [, volume] [, loop]) → bool
//     经 EngineDLL AudioPlay → 宿主回调（Player=AVAudioPlayer；编辑器可注册
//     预听桥）。路径解析归宿主（Player 的包内相对三级解析）。
//   audio.set_group(group, gain) / audio.set_master(gain) → bool（0..1）
// 与 InputLua/NavLua 同构（extra_globals_registrar 单份实现）。

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace primal::scripting {

// EngineDLL AudioAPI 的两个落点（宿主链接注入；实现见 AudioAPI.cpp——这里
// 只声明签名，链接期解析，避免 Scripting → EngineDLL 反向依赖）。
// 为保持 Engine/Scripting 纯净，本头通过函数指针表由注册方注入实现。
struct audio_api_bridge
{
    unsigned (*play)(const char* path, const char* group, float volume, float pan, unsigned loop);
    unsigned (*set_group)(const char* group, float gain);
    unsigned (*set_master)(float gain);
};

// 注入实现（EngineDLL ScriptAPI 初始化时调用一次；nullptr 项 = 功能缺席，
// audio.play 恒 false 而非崩溃）。
inline audio_api_bridge g_audio_bridge{};

inline int lua_audio_play(lua_State* L)
{
    const char* path = lua_tostring(L, 1);
    const char* group = lua_tostring(L, 2);
    const float volume = lua_gettop(L) >= 3 ? (float)lua_tonumber(L, 3) : 1.f;
    const unsigned loop = lua_gettop(L) >= 4 ? (unsigned)lua_tointeger(L, 4) : 0u;
    if (!path || !g_audio_bridge.play)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_audio_bridge.play(path, group, volume, 0.f, loop) ? 1 : 0);
    return 1;
}

inline int lua_audio_set_group(lua_State* L)
{
    const char* group = lua_tostring(L, 1);
    const float gain = (float)luaL_checknumber(L, 2);
    if (!group || !g_audio_bridge.set_group)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_audio_bridge.set_group(group, gain) ? 1 : 0);
    return 1;
}

inline int lua_audio_set_master(lua_State* L)
{
    const float gain = (float)luaL_checknumber(L, 1);
    if (!g_audio_bridge.set_master)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_audio_bridge.set_master(gain) ? 1 : 0);
    return 1;
}

inline void register_audio_table(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, &lua_audio_play);    lua_setfield(L, -2, "play");
    lua_pushcfunction(L, &lua_audio_set_group); lua_setfield(L, -2, "set_group");
    lua_pushcfunction(L, &lua_audio_set_master); lua_setfield(L, -2, "set_master");
    lua_setglobal(L, "audio");
}

} // namespace primal::scripting
