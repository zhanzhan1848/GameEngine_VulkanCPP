// ============================================================================
// ScriptAPI — 生产脚本宿主导出（scripting-properties-and-events S1, 2026-10-11）
//
// 引擎脚本框架（Engine/Components/Script*）此前只对静态链接 Engine 的测试可
// 执行文件可用：script ABI 未导出（-fvisibility=hidden）、EngineDLL 无 Script
// 源文件、PipelineTick 不调 script::frame_tick。本文件把 Lua 后端
// （Engine/Scripting/LuaBackend，自测试树生产化）接到编辑器/Player 宿主。
//
// 失败语义（子规划 S1 合同）：
//   - 坏 Lua / 缺文件：返回 invalid，stderr 定位（实体/文件/行），宿主进程
//     不退出；已运行的其它实例不受影响。
//   - ScriptSetParamsForEntity 在 S1 以"同实体销毁重建"实现——返回新 handle，
//     调用方必须改用返回值（旧 handle 作废）。
// ============================================================================

#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "../Engine/Components/Script.h"
#include "../Engine/Components/ScriptFilesystem.h"
#include "../Engine/Scripting/LuaBackend.h"
#include "../Engine/Scripting/InputState.h"
#include "../Engine/Scripting/InputLua.h"
#include "../Engine/Scripting/NavLua.h"
#include "../Engine/Scripting/AudioLua.h"

// audio S2：EngineDLL AudioAPI 导出（同 dylib 内直链——extern "C" 转换单元可见）
extern "C" {
unsigned AudioPlay(const char* path, const char* group, float voice_gain, float pan, unsigned loop);
unsigned AudioSetGroupGain(const char* group, float gain);
unsigned AudioSetMasterGain(float gain);
}
#include "../Engine/Components/Entity.h"
#include "../Engine/Components/Transform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace id = primal::id;
namespace math = primal::math;
namespace game_entity = primal::game_entity;
namespace transform = primal::transform;

namespace {

constexpr u64 kInvalidScriptHandle = static_cast<u64>(id::invalid_id);

// ---------------------------------------------------------------------------
// 实体变换助手 —— 与 EntityAPI.cpp 的 Set/GetEntityTransform 同一数学与单位
// 语义（旋转 = 欧拉弧度 pitch/yaw/roll）。self 表的 Lua 侧契约由此冻结：
// self.rotation()/set_rotation() 与 entity.set-transform CLI/导出包 rot 行
// 完全同数值。
// ---------------------------------------------------------------------------
game_entity::entity script_entity_from_id(id::id_type id)
{
    return game_entity::entity{ game_entity::entity_id(id) };
}

math::v4 script_euler_to_quat(const f32* rotation)
{
#if defined(_MSC_VER)
    using namespace DirectX;
    XMFLOAT3A rot{ rotation[0], rotation[1], rotation[2] };
    XMVECTOR quat{ XMQuaternionRotationRollPitchYawFromVector(XMLoadFloat3A(&rot)) };
    XMFLOAT4A result{};
    XMStoreFloat4A(&result, quat);
    return math::v4{ result.x, result.y, result.z, result.w };
#elif defined(__clang__)
    using namespace simd;
    simd::quatf quat_x = simd_quaternion(rotation[0], simd_make_float3(1.0f, 0.0f, 0.0f));
    simd::quatf quat_y = simd_quaternion(rotation[1], simd_make_float3(0.0f, 1.0f, 0.0f));
    simd::quatf quat_z = simd_quaternion(rotation[2], simd_make_float3(0.0f, 0.0f, 1.0f));
    simd::quatf quat = simd_mul(simd_mul(quat_z, quat_y), quat_x);
    return math::v4{ quat.vector.x, quat.vector.y, quat.vector.z, quat.vector.w };
#else
    // 数学期等价的手写欧拉→四元数（GLM 不可用时保持自包含）。
    const f32 cx{ cosf(rotation[0] * 0.5f) }, sx{ sinf(rotation[0] * 0.5f) };
    const f32 cy{ cosf(rotation[1] * 0.5f) }, sy{ sinf(rotation[1] * 0.5f) };
    const f32 cz{ cosf(rotation[2] * 0.5f) }, sz{ sinf(rotation[2] * 0.5f) };
    return math::v4{
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz };
#endif
}

void script_quat_to_euler(math::v4 q, f32* out)
{
    f32 sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
    f32 cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    out[0] = atan2f(sinr_cosp, cosr_cosp);
    f32 sinp = 2.0f * (q.w * q.y - q.z * q.x);
    sinp = sinp > 1.0f ? 1.0f : (sinp < -1.0f ? -1.0f : sinp);
    out[1] = asinf(sinp);
    f32 siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    f32 cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    out[2] = atan2f(siny_cosp, cosy_cosp);
}

// 当前实例的 entity id：LuaBackend 在每次钩子派发前把 `entity_id` Lua 全局
// 重设为当前实例（LuaWriterIdScope 契约）。self 表从这里取——不引入第二份
// 实例上下文。
id::id_type self_current_entity(lua_State* L)
{
    lua_getglobal(L, "entity_id");
    id::id_type eid = id::invalid_id;
    if (lua_type(L, -1) == LUA_TNUMBER)
        eid = static_cast<id::id_type>((u64)lua_tonumber(L, -1));
    lua_pop(L, 1);
    return eid;
}

// --- self 表：读 ---

int self_position(lua_State* L)
{
    game_entity::entity e{ script_entity_from_id(self_current_entity(L)) };
    if (!e.is_valid()) { lua_pushnil(L); return 1; }
    const math::v3 p{ e.position() };
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}

int self_rotation(lua_State* L)
{
    game_entity::entity e{ script_entity_from_id(self_current_entity(L)) };
    if (!e.is_valid()) { lua_pushnil(L); return 1; }
    f32 r[3];
    script_quat_to_euler(e.rotation(), r);
    lua_pushnumber(L, r[0]); lua_pushnumber(L, r[1]); lua_pushnumber(L, r[2]);
    return 3;
}

int self_scale(lua_State* L)
{
    game_entity::entity e{ script_entity_from_id(self_current_entity(L)) };
    if (!e.is_valid()) { lua_pushnil(L); return 1; }
    const math::v3 s{ e.scale() };
    lua_pushnumber(L, s.x); lua_pushnumber(L, s.y); lua_pushnumber(L, s.z);
    return 3;
}

// --- self 表：写（走 transform::component_cache，同 EntityAPI 路径） ---

void self_write_transform(lua_State* L, bool pos, bool rot, bool scl)
{
    f32 p[3]{ 0, 0, 0 }, r[3]{ 0, 0, 0 }, s[3]{ 1, 1, 1 };
    if (pos) { p[0] = (f32)luaL_checknumber(L, 1); p[1] = (f32)luaL_checknumber(L, 2); p[2] = (f32)luaL_checknumber(L, 3); }
    if (rot) { r[0] = (f32)luaL_checknumber(L, 1); r[1] = (f32)luaL_checknumber(L, 2); r[2] = (f32)luaL_checknumber(L, 3); }
    if (scl) { s[0] = (f32)luaL_checknumber(L, 1); s[1] = (f32)luaL_checknumber(L, 2); s[2] = (f32)luaL_checknumber(L, 3); }

    const id::id_type eid = self_current_entity(L);
    game_entity::entity e{ script_entity_from_id(eid) };
    if (!e.is_valid()) {
        std::fprintf(stderr, "[ScriptAPI] self write: entity %llu invalid\n",
                     (unsigned long long)eid);
        return;
    }

    transform::component_cache cache{};
    cache.id = transform::transform_id{ eid };
    u32 flags = 0;
    if (pos) { cache.position = math::v3{ p[0], p[1], p[2] }; flags |= transform::component_flags::position; }
    if (rot) { cache.rotation = script_euler_to_quat(r);            flags |= transform::component_flags::rotation; }
    if (scl) { cache.scale    = math::v3{ s[0], s[1], s[2] };       flags |= transform::component_flags::scale; }
    if (flags) {
        cache.flags = flags;
        transform::update(&cache, 1);
    }
}

int self_set_position(lua_State* L) { self_write_transform(L, true,  false, false); return 0; }
int self_set_rotation(lua_State* L) { self_write_transform(L, false, true,  false); return 0; }
int self_set_scale(lua_State* L)    { self_write_transform(L, false, false, true);  return 0; }

// input-camera S1：脚本侧动作查询表（实现共用 Engine/Scripting/InputLua.h，
// 测试树同源验证）。动作状态由宿主经 ScriptSetInputState 每帧写入
//（键位→动作的映射归宿主，引擎不感知具体设备/键位）。

// LuaBackend 的每类型额外全局注册器：entity 表（读 3 值 / 写 3 参数，弧度）。
// 命名注意：不能用 `self`——Lua 方法语法（function M:update）的接收者恰好
// 叫 self，会把本全局遮蔽（实测踩坑）。
void register_script_globals(void* lua_state)
{
    lua_State* L = static_cast<lua_State*>(lua_state);
    lua_newtable(L);
    lua_pushcfunction(L, self_position);      lua_setfield(L, -2, "position");
    lua_pushcfunction(L, self_set_position);  lua_setfield(L, -2, "set_position");
    lua_pushcfunction(L, self_rotation);      lua_setfield(L, -2, "rotation");
    lua_pushcfunction(L, self_set_rotation);  lua_setfield(L, -2, "set_rotation");
    lua_pushcfunction(L, self_scale);         lua_setfield(L, -2, "scale");
    lua_pushcfunction(L, self_set_scale);     lua_setfield(L, -2, "set_scale");
    lua_setglobal(L, "entity");
    primal::scripting::register_input_table(L);
    // ai-navigation S1：脚本侧导航任务表（同一运行态，宿主/脚本共用）
    primal::scripting::register_nav_table(L);
    // audio S2：脚本侧音频事件表（播放经宿主回调——引擎不持设备）
    primal::scripting::g_audio_bridge.play = &AudioPlay;
    primal::scripting::g_audio_bridge.set_group = &AudioSetGroupGain;
    primal::scripting::g_audio_bridge.set_master = &AudioSetMasterGain;
    primal::scripting::register_audio_table(L);
}

// ---------------------------------------------------------------------------
// 类型/实例登记（路径 → type_id 缓存；handle → 实体，供 SetParams 重建）
// ---------------------------------------------------------------------------
std::unordered_map<std::string, u64> g_type_by_path;
std::unordered_map<u64, id::id_type> g_entity_by_script;
std::mutex g_script_mutex;
bool g_systems_ready = false;

u64 find_or_register_type(const char* lua_path)
{
    std::string key{ lua_path };
    if (const auto it = g_type_by_path.find(key); it != g_type_by_path.end())
        return it->second;

    // type_name 用规范化路径——跨运行稳定（ScriptState types.<T> 命名空间
    // 需要），同文件多实例共享一个 lua_State/类型槽。
    u64 type_id = primal::lua_backend::LuaBackend::instance().register_type(key.c_str(), lua_path);
    if (type_id == u64_invalid_id) return u64_invalid_id;
    g_type_by_path[key] = type_id;
    return type_id;
}

} // namespace

extern "C" {

// 幂等初始化：script 子系统 + Lua 后端 + self 全局注册器。
// PipelineTick 的 script 段以 is_initialized 守卫——本函数没跑过的进程
// （如无脚本组件的编辑器会话）frame_tick 直接跳过。
EDITOR_INTERFACE u32 ScriptEnsureSystems()
{
    std::lock_guard<std::mutex> lock(g_script_mutex);
    if (g_systems_ready) return 1u;
    primal::script::initialize();
    if (!primal::script::is_initialized()) {
        std::fprintf(stderr, "[ScriptAPI] script::initialize failed\n");
        return 0u;
    }
    // 沙箱文件系统默认根（宿主 cwd 相对；构造器不设路径，宿主不设则 file.*
    // 全部失败）。Data 必须是窄目录而非 "."——根匹配按 Data→Save→Log→Cache
    // 顺序前缀比对，"." 会把所有路径先吸进只读 Data 根（log:// 拒写实测）。
    {
        namespace fs = std::filesystem;
        auto& sandbox = primal::script::filesystem();
        using RK = primal::script::RootKind;
        fs::create_directories("./ScriptsData");
        fs::create_directories("./ScriptsSave");
        fs::create_directories("./ScriptsLog");
        fs::create_directories("./ScriptsCache");
        sandbox.set_root(RK::Data, "./ScriptsData");
        sandbox.set_root(RK::Save, "./ScriptsSave");
        sandbox.set_root(RK::Log, "./ScriptsLog");
        sandbox.set_root(RK::Cache, "./ScriptsCache");
    }
    auto& backend = primal::lua_backend::LuaBackend::instance();
    backend.initialize();
    backend.set_extra_globals_registrar(&register_script_globals);
    g_systems_ready = true;
    return 1u;
}

// 给实体挂 Lua 脚本实例。lua_path 绝对或进程 cwd 相对（宿主自行解析——
// 编辑器传 cooked/源路径，Player 传包解析出的绝对路径）。params 为并行数组。
// 成功返回 script handle；失败返回 0xFFFFFFFF（含坏脚本/缺文件——stderr 定位，
// 宿主不退出）。
EDITOR_INTERFACE u64 ScriptCreateForEntity(u32 entity_id, const char* lua_path,
                                           const char** param_keys,
                                           const char** param_values,
                                           u32 param_count)
{
    if (ScriptEnsureSystems() == 0u) return kInvalidScriptHandle;
    if (!lua_path || !lua_path[0] || entity_id == id::invalid_id) return kInvalidScriptHandle;

    std::lock_guard<std::mutex> lock(g_script_mutex);
    const u64 type_id = find_or_register_type(lua_path);
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "[ScriptAPI] script type registration failed: %s\n", lua_path);
        return kInvalidScriptHandle;
    }

    const u64 script_id = primal::lua_backend::LuaBackend::instance().create_instance(
        type_id, entity_id, param_keys, param_values, param_count);
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "[ScriptAPI] create_instance failed: entity=%u path=%s\n",
                     entity_id, lua_path);
        return kInvalidScriptHandle;
    }
    g_entity_by_script[script_id] = entity_id;
    return script_id;
}

// 销毁实例（幂等，invalid 容忍）。引擎侧 remove_for_entity 触发后端 destroy
// 钩子并解绑事件订阅。
EDITOR_INTERFACE void ScriptDestroyForEntity(u64 script_id)
{
    if (script_id == kInvalidScriptHandle) return;
    std::lock_guard<std::mutex> lock(g_script_mutex);
    const auto it = g_entity_by_script.find(script_id);
    if (it == g_entity_by_script.end()) return;
    if (primal::script::is_initialized())
        primal::script::remove_for_entity(game_entity::entity_id(it->second));
    g_entity_by_script.erase(it);
}

// input-camera S1：宿主每帧写入动作状态（键位→动作映射在宿主侧完成）。
// 脚本经 input.is_down(action) 读取。不要求 ScriptEnsureSystems 先行——
// InputState 独立于脚本子系统（无脚本的宿主也可写）。
EDITOR_INTERFACE void ScriptSetInputState(const char* action, u32 down)
{
    if (!action || !*action) return;
    primal::scripting::InputState::instance().set(action, down != 0u);
}

// S1 实现 = 同实体销毁重建（begin_play 会以新 params 重放）。返回新 handle；
// 旧 handle 作废。引擎每实体仅一个脚本组件（entity_to_script 单射），故必须
// 先拆旧再建新；create_instance 在类型已注册后不再解析 Lua 源，失败只剩分配
// 级错误（实践不可达），不构成"坏脚本丢旧实例"路径——源码级失败在
// find_or_register_type 已被旧实例保留语义覆盖。
EDITOR_INTERFACE u64 ScriptSetParamsForEntity(u64 script_id,
                                              const char** param_keys,
                                              const char** param_values,
                                              u32 param_count)
{
    if (script_id == kInvalidScriptHandle) return kInvalidScriptHandle;
    std::lock_guard<std::mutex> lock(g_script_mutex);
    const auto it = g_entity_by_script.find(script_id);
    if (it == g_entity_by_script.end()) return kInvalidScriptHandle;
    const id::id_type entity = it->second;

    auto* inst = primal::lua_backend::LuaBackend::instance().find_instance(script_id);
    if (!inst || !inst->type) return kInvalidScriptHandle;
    const u64 type_id = inst->type->type_id;

    primal::script::remove_for_entity(game_entity::entity_id(entity));
    g_entity_by_script.erase(it);

    const u64 fresh = primal::lua_backend::LuaBackend::instance().create_instance(
        type_id, entity, param_keys, param_values, param_count);
    if (fresh == u64_invalid_id) {
        std::fprintf(stderr, "[ScriptAPI] set-params recreate failed (entity=%llu)\n",
                     (unsigned long long)entity);
        return kInvalidScriptHandle;
    }
    g_entity_by_script[fresh] = entity;
    return fresh;
}

} // extern "C"
