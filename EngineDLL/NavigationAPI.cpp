// NavigationAPI.cpp — ai-navigation S1 收口（2026-10-15）：EngineDLL 导出面。
//
// 纪律：导出只增不改（AGENTS ABI 纪律）；函数语义与
// Engine/Navigation/NavigationRuntime.h 一一对应。PipelineTick 的 nav 段
// 在 PhysicsAPI.cpp（帧序契约：script → nav → physics → pfx → bindings）。
// 无 navagent 的进程（编辑器无组件/无烘焙）这些函数从不被调，tick 空转。

#include "Common.h"
#include "CommonHeaders.h"
#include "Navigation/NavigationRuntime.h"

extern "C" {

// --- 全局资产 -----------------------------------------------------------

// 用三角汤（xyz 三元组 + 索引）烘焙内存导航资产。profile 为 4 浮数
// [radius, height, max_slope_deg, step_height]，空指针用引擎缺省。
// 返回 nav_error（0=ok）；失败保持原资产。
EDITOR_INTERFACE u32 NavigationBake(const f32* positions, u32 position_count,
                                    const u32* indices, u32 index_count,
                                    const f32* profile4)
{
    if (!positions || !indices) return (u32)primal::navigation::nav_error::null_input;
    primal::navigation::agent_profile p{};
    if (profile4) {
        p.radius = profile4[0];
        p.height = profile4[1];
        p.max_slope_deg = profile4[2];
        p.step_height = profile4[3];
        if (!p.valid()) return (u32)primal::navigation::nav_error::null_input;
    }
    const primal::navigation::bake_input in{
        positions, position_count, indices, index_count, 0.25f, 0.05f, 0.5f
    };
    return primal::navigation::runtime::bake(in, p)
        ? (u32)primal::navigation::nav_error::ok
        : (u32)primal::navigation::nav_error::internal;
}

// 加载烘焙文件（NavigationService 确定性文本格式；坏文件类型化失败）。
EDITOR_INTERFACE u32 NavigationLoadAsset(const char* path)
{
    return primal::navigation::runtime::load_asset_file(path)
        ? (u32)primal::navigation::nav_error::ok
        : (u32)primal::navigation::nav_error::bad_format;
}

EDITOR_INTERFACE u32 NavigationHasAsset(void)
{
    return primal::navigation::runtime::has_asset() ? 1u : 0u;
}

EDITOR_INTERFACE u32 NavigationGeneration(void)
{
    return primal::navigation::runtime::generation();
}

EDITOR_INTERFACE void NavigationClear(void)
{
    primal::navigation::runtime::clear();
}

// --- 任务面（宿主直调；脚本侧走 Lua nav 表同一运行态）-------------------

// 为实体建/换移动任务。target3 = xyz；speed<=0 用缺省 2m/s。返回 nav_error。
EDITOR_INTERFACE u32 NavigationMoveTo(u64 entity, const f32* target3, f32 speed)
{
    if (!target3) return (u32)primal::navigation::nav_error::null_input;
    return (u32)primal::navigation::runtime::move_to(
        entity, primal::math::v3{ target3[0], target3[1], target3[2] }, speed);
}

EDITOR_INTERFACE void NavigationStop(u64 entity)
{
    primal::navigation::runtime::stop(entity);
}

// 无任务 = idle(0)；moving=1 completed=2 cancelled=3 failed=4（move_state）。
EDITOR_INTERFACE u32 NavigationState(u64 entity)
{
    return (u32)primal::navigation::runtime::state_of(entity);
}

// 无任务 = ok；失败任务携带类型化码（stale_nav/invalid_target/unreachable...）。
EDITOR_INTERFACE u32 NavigationFailure(u64 entity)
{
    return (u32)primal::navigation::runtime::failure_of(entity);
}

// 实体销毁反注册（与 PhysicsRemoveEntityCollider 同生命周期语义）。
EDITOR_INTERFACE void NavigationRemoveEntity(u64 entity)
{
    primal::navigation::runtime::remove_entity(entity);
}

} // extern "C"
