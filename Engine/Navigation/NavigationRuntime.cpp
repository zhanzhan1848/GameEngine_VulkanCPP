// NavigationRuntime.cpp — 导航运行态实现（ai-navigation S1 收口）。

#include "Navigation/NavigationRuntime.h"
#include "Common/CoreLogStream.h"
#include "EngineAPI/GameEntity.h"
#include "Components/Transform.h"

#include <cmath>
#include <unordered_map>

namespace primal::navigation::runtime {

namespace {

navigation_asset g_asset;
bool g_has_asset = false;
std::unordered_map<u64, move_task> g_tasks;

// 实体 id 在引擎内是 32 位槽位（id::id_type）；u64 键来自宿主/Lua 全局
// entity_id（PhysicsAPI 的 id::index 同一口径），值域一致。
game_entity::entity entity_from_raw(u64 raw)
{
    return game_entity::entity{ game_entity::entity_id{ static_cast<id::id_type>(raw) } };
}

} // anonymous namespace

bool bake(const bake_input& in, const agent_profile& p)
{
    if (asset_bake(g_asset, in, p) != nav_error::ok) return false;
    g_has_asset = true;
    return true;
}

bool load_asset_file(const char* path)
{
    if (asset_load(g_asset, path) != nav_error::ok) return false;
    g_has_asset = true;
    return true;
}

bool has_asset() { return g_has_asset && !g_asset.grid.empty(); }

u32 generation() { return g_asset.generation; }

nav_error move_to(u64 entity_id, const math::v3& target, f32 speed)
{
    if (!has_asset()) {
        g_tasks.erase(entity_id);   // 不留半成品：无资产时语义 = 立即可判失败
        return nav_error::no_nav_data;
    }
    const game_entity::entity e = entity_from_raw(entity_id);
    if (!e.is_valid()) return nav_error::invalid_target;

    move_task& t = g_tasks[entity_id];
    const nav_error err = task_create(g_asset, e.position(), target,
                                      speed > 0.f ? speed : 2.f, 1.f, t);
    if (err != nav_error::ok && t.state != move_state::failed) {
        // task_create 契约：失败时任务已置 failed + failure 码；防御分支
        g_tasks.erase(entity_id);
        return err;
    }
    if (err != nav_error::ok)
        PRIMAL_LOG_STREAM(Warn, "Navigation")
            << "[NavRuntime] move_to entity=" << entity_id
            << " failed: " << to_string(err) << std::endl;
    return err;
}

void stop(u64 entity_id)
{
    if (const auto it = g_tasks.find(entity_id); it != g_tasks.end())
        task_cancel(it->second);
}

move_state state_of(u64 entity_id)
{
    const auto it = g_tasks.find(entity_id);
    return it == g_tasks.end() ? move_state::idle : it->second.state;
}

nav_error failure_of(u64 entity_id)
{
    const auto it = g_tasks.find(entity_id);
    return it == g_tasks.end() ? nav_error::ok : it->second.failure;
}

void tick(f32 dt)
{
    if (g_tasks.empty()) return;
    if (!has_asset()) {
        g_tasks.clear();   // 资产被清空：任务全部作废（宿主 teardown 顺序兜底）
        return;
    }
    for (auto it = g_tasks.begin(); it != g_tasks.end();) {
        const game_entity::entity e = entity_from_raw(it->first);
        if (!e.is_valid()) {
            it = g_tasks.erase(it);
            continue;
        }
        move_task& t = it->second;
        const math::v3 cur = e.position();
        math::v3 out = cur;
        const move_event ev = task_update(g_asset, t, cur, dt, out);
        const bool moved = fabsf(out.x - cur.x) + fabsf(out.y - cur.y) + fabsf(out.z - cur.z) > 1e-9f;
        if (moved) {
            // transform 回写走组件通道（与 PhysicsWorld 的 owner 回写同一路径）
            primal::transform::component tc{ primal::transform::transform_id{ e.get_id() } };
            primal::transform::set_position(tc, out);
        }
        if (ev == move_event::arrived)
            PRIMAL_LOG_STREAM(Info, "Navigation")
                << "[NavRuntime] entity=" << it->first << " arrived" << std::endl;
        else if (ev == move_event::failed_stale)
            PRIMAL_LOG_STREAM(Warn, "Navigation")
                << "[NavRuntime] entity=" << it->first
                << " stale nav (rebaked?) — task failed" << std::endl;
        ++it;
    }
}

void remove_entity(u64 entity_id) { g_tasks.erase(entity_id); }

void clear()
{
    g_tasks.clear();
    g_asset = navigation_asset{};
    g_has_asset = false;
}

} // namespace primal::navigation::runtime
