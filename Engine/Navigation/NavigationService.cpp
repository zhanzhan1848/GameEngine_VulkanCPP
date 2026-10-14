// NavigationService.cpp — 导航资产外壳与移动任务生命周期（ai-navigation S1）。

#include "Navigation/NavigationService.h"
#include "Common/CoreLogStream.h"

#include <atomic>
#include <cstdio>

namespace primal::navigation {

namespace {

std::atomic<u64> g_next_task_id{ 1 };

inline f32 v3_dist(const math::v3& a, const math::v3& b)
{
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

} // anonymous namespace

nav_error asset_bake(navigation_asset& a, const bake_input& in, const agent_profile& p)
{
    nav_grid grid;
    const nav_error e = bake(in, p, grid);
    if (e != nav_error::ok) return e;   // 失败保持原状
    a.grid = std::move(grid);
    a.generation += 1;
    return nav_error::ok;
}

nav_error asset_save(const navigation_asset& a, const char* path)
{
    if (!path || !path[0]) return nav_error::null_input;
    std::string text;
    const nav_error e = serialize(a.grid, text);
    if (e != nav_error::ok) return e;

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return nav_error::bad_format;
    const size_t written = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (written != text.size()) return nav_error::bad_format;
    return nav_error::ok;
}

nav_error asset_load(navigation_asset& a, const char* path)
{
    if (!path || !path[0]) return nav_error::null_input;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return nav_error::bad_format;
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    nav_grid grid;
    const nav_error e = deserialize(text.c_str(), text.size(), grid);
    if (e != nav_error::ok) return e;
    a.grid = std::move(grid);
    a.generation += 1;
    return nav_error::ok;
}

nav_error task_create(const navigation_asset& a, const math::v3& from, const math::v3& to,
                      f32 speed, f32 nearest_radius, move_task& out)
{
    out = move_task{};
    out.id = g_next_task_id.fetch_add(1);
    out.target = to;
    out.speed = speed > 0.f ? speed : 2.f;
    out.generation = a.generation;

    if (a.grid.empty()) {
        out.state = move_state::failed;
        out.failure = nav_error::no_nav_data;
        return nav_error::no_nav_data;
    }
    const path_result res = find_path(a.grid, from, to, nearest_radius);
    if (res.error != nav_error::ok || res.waypoints.empty()) {
        out.state = move_state::failed;
        out.failure = res.error != nav_error::ok ? res.error : nav_error::internal;
        return out.failure;
    }
    out.path = std::move(res.waypoints);
    out.state = move_state::moving;
    out.next_waypoint = 1;
    // 起点即终点的退化任务：直接完成并占用唯一事件位
    if (out.path.size() < 2 || v3_dist(out.path.front(), out.path.back()) <= out.arrival_radius) {
        out.state = move_state::completed;
        out.arrived_event_fired = true;
    }
    return nav_error::ok;
}

move_event task_update(const navigation_asset& a, move_task& t, const math::v3& current,
                       f32 dt, math::v3& out_pos)
{
    out_pos = current;
    if (t.state != move_state::moving) return move_event::none;

    // 合同：陈旧数据提示重烘焙（任务代际 != 资产代际）
    if (t.generation != a.generation) {
        t.state = move_state::failed;
        t.failure = nav_error::stale_nav;
        return move_event::failed_stale;
    }
    if (dt <= 0.f || t.speed <= 0.f || t.path.size() < 2) return move_event::none;

    // 沿航点折线推进 speed*dt
    f32 budget = t.speed * dt;
    math::v3 pos = current;
    while (t.next_waypoint < t.path.size() && budget > 0.f) {
        const math::v3& wp = t.path[t.next_waypoint];
        const f32 d = v3_dist(pos, wp);
        if (d <= budget) {
            pos = wp;
            budget -= d;
            ++t.next_waypoint;
        } else {
            const f32 k = budget / d;
            pos = math::v3{ pos.x + (wp.x - pos.x) * k,
                            pos.y + (wp.y - pos.y) * k,
                            pos.z + (wp.z - pos.z) * k };
            budget = 0.f;
        }
    }
    out_pos = pos;

    const math::v3& final_wp = t.path.back();
    if (t.next_waypoint >= t.path.size() || v3_dist(pos, final_wp) <= t.arrival_radius) {
        out_pos = final_wp;   // 到达：吸附终点（合同 A01 ≤0.1m）
        t.state = move_state::completed;
        if (!t.arrived_event_fired) {
            t.arrived_event_fired = true;
            return move_event::arrived;
        }
    }
    return move_event::none;
}

void task_cancel(move_task& t)
{
    if (t.state == move_state::moving) t.state = move_state::cancelled;
    else if (t.state == move_state::idle) t.state = move_state::cancelled;
}

} // namespace primal::navigation
