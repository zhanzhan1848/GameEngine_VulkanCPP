#pragma once

// NavigationTypes.h — 基础 AI 与导航的公共类型（ai-navigation S1）。
//
// 纪律：本头文件只使用引擎基础类型，不依赖物理/Jolt/渲染头，
// 可被 Engine 内部、EngineDLL、测试与工具层安全包含。
// 单位约定：米、秒、弧度，右手系 Y 向上（与 SimulationAPI.h 一致）。
// 阶段合同：Docs/plans/production/ai-navigation.md（S1 = 烘焙/路径/跟随/取消/失败诊断）。

#include "CommonHeaders.h"

namespace primal::navigation {

// ---- 失败语义（合同要求：无nav/尺寸过大/不可达分开，不静默）----
enum class nav_error : u32 {
    ok                 = 0,
    null_input         = 1,  // 烘焙输入空指针/索引数非 3 倍/参数非有限或非正
    bad_format         = 2,  // 反序列化数据损坏（magic/计数/结构不一致）
    unsupported_version= 3,  // schema 版本不认识（不静默套用）
    no_nav_data        = 4,  // 查询侧没有可用网格（未烘焙/空资产）
    agent_too_large    = 5,  // 该 AgentProfile 腐蚀后没有任何可走单元
    invalid_target     = 6,  // 起点或目标附近（半径内）找不到可走面
    unreachable        = 7,  // 可走但与起点不在同一连通域
    stale_nav          = 8,  // 任务引用的网格代际已过期（提示重烘焙）
    internal           = 9   // 不变量破坏（防御性，正常不应出现）
};

inline const char* to_string(nav_error e)
{
    switch (e) {
        case nav_error::ok:                  return "ok";
        case nav_error::null_input:          return "null_input";
        case nav_error::bad_format:          return "bad_format";
        case nav_error::unsupported_version: return "unsupported_version";
        case nav_error::no_nav_data:         return "no_nav_data";
        case nav_error::agent_too_large:     return "agent_too_large";
        case nav_error::invalid_target:      return "invalid_target";
        case nav_error::unreachable:         return "unreachable";
        case nav_error::stale_nav:           return "stale_nav";
        case nav_error::internal:            return "internal";
    }
    return "unknown";
}

// ---- 角色外形（与物理碰撞尺度一致的合同要求）----
struct agent_profile {
    f32 radius       { 0.4f };   // 水平胶囊半径（米），用于网格腐蚀
    f32 height       { 1.8f };   // 净空要求（米），span 上方间隙不足判不可走
    f32 max_slope_deg{ 45.f };   // 可走坡度上限（度）
    f32 step_height  { 0.35f };  // 台阶/连接判定高度差（米）

    bool valid() const
    {
        return radius > 0.f && height > 0.f && step_height >= 0.f
            && max_slope_deg > 0.f && max_slope_deg < 90.f
            && std::isfinite(radius) && std::isfinite(height)
            && std::isfinite(max_slope_deg) && std::isfinite(step_height);
    }
};

// ---- 烘焙输入：抽象三角汤（场景接线时由调用方从碰撞几何/网格资产组装）----
struct bake_input {
    const f32* positions   { nullptr };  // xyz 三元组
    u32        position_count{ 0 };
    const u32* indices      { nullptr };  // 三角形索引
    u32        index_count  { 0 };         // 必须是 3 的倍数
    f32        cell_size    { 0.25f };     // XZ 列尺寸（米）
    f32        cell_height  { 0.05f };     // span 合并容差（米）
    f32        bounds_padding{ 0.5f };     // 包围盒外扩（米）
};

// ---- 移动任务状态机（idle→moving→completed；任意态可 cancelled；失败态由 failure 携带）----
enum class move_state : u8 { idle = 0, moving, completed, cancelled, failed };

// task_update 单次返回的事件；arrived 在任务生命周期内恰好触发一次（合同 A01）。
enum class move_event : u8 { none = 0, arrived, cancelled_confirm, failed_stale };

} // namespace primal::navigation
