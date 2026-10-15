#pragma once

// SimulationAPI.h — 引擎物理模拟层的唯一对外头文件。
//
// 纪律：Jolt 头文件不允许出现在 Engine/Simulation/ 之外
// （Docs/2026-09-11-physics-simulation-design.md §0），本头文件只使用引擎类型，
// 可被 EngineDLL / 测试 / 组件层安全包含。单位约定：米、秒、弧度，右手系 Y 向上，
// 与引擎 Transform 组件一致。

#include "CommonHeaders.h"
#include "EngineAPI/GameEntity.h"

namespace primal::simulation {

// ---- 对象层（碰撞过滤策略的引擎侧事实源；Jolt 侧过滤表在 Initialize 时按 can_collide 逐对启用）----
namespace layers {
constexpr u32 kStatic   = 0;
constexpr u32 kDynamic  = 1;
constexpr u32 kDebris   = 2;
constexpr u32 kTrigger  = 3;
constexpr u32 kParticle = 4;   // 刚体化粒子(P2):碰世界、不碰同类(自碰关,路线图 P3+)
constexpr u32 kCount    = 5;

// broadphase 层数：static 类（static/trigger 归 0）与 moving 类（dynamic/debris 归 1）
constexpr u32 kBroadPhaseStatic = 0;
constexpr u32 kBroadPhaseMoving = 1;
constexpr u32 kBroadPhaseCount  = 2;

// 纯函数：两对象层是否互相碰撞。L0 单测直接测它；
// body 的 broadphase 归属见 broad_phase_layer_for()。
bool can_collide(u32 a, u32 b);
u32  broad_phase_layer_for(u32 object_layer);
} // namespace layers

// ---- 固定步长累加器（纯逻辑，L0 单测覆盖；Gaffer on Games "Fix Your Timestep" 变体）----
struct fixed_step_accumulator {
    f32 fixed_dt     { 1.f / 60.f };
    u32 max_sub_steps{ 5 };
    f32 accumulator  { 0.f };

    // 推进 host_dt，返回本次应执行的物理子步数。
    // host_dt 先被钳到 0.25s（防死亡螺旋）；连续追不上时清零余量，不让误差跨帧累积。
    u32 advance(f32 host_dt);
    void reset() { accumulator = 0.f; }
};

struct settings {
    math::v3 gravity          { 0.f, -9.81f, 0.f };
    u32      max_bodies       { 4096 };
    u32      max_worker_threads{ 0 };   // 0 = 单线程（M0/M1 确定性基线；M2 接 JobSystem 适配器）
    f32      fixed_dt         { 1.f / 60.f };
    u32      max_sub_steps    { 5 };
};

struct body_desc {
    enum class shape : u32 { box = 0, sphere = 1 };
    enum class mode  : u32 { static_body = 0, dynamic = 1, kinematic = 2 };

    shape    s{ shape::box };
    mode     m{ mode::dynamic };
    math::v3 half_extents{ 0.5f, 0.5f, 0.5f };   // box：半尺寸；sphere：x 分量为半径
    math::v3 position{ 0.f, 0.f, 0.f };
    math::v3 orientation_euler{ 0.f, 0.f, 0.f }; // 弧度，XYZ 顺序
    f32      mass      { 1.f };
    f32      restitution{ 0.1f };
    f32      friction  { 0.5f };
    u32      layer     { layers::kDynamic };
    game_entity::entity owner{};                 // M1：绑定 ECS 实体后由物理 tick 回写 transform
    bool     debug_draw_proxy{ true };            // 是否画线框 proxy（可视化测试）
};

struct body_transform {
    math::v3 position{};
    math::v3 orientation_euler{};
    math::v3 linear_velocity{};
    bool     sleeping{ false };
};

// ---- world 门面（自由函数；实现持全局单例于 PhysicsWorld.cpp）----
bool     initialize(const settings& s);
void     shutdown();
bool     is_initialized();
void     set_running(bool running);
bool     running();
void     step(f32 host_dt);          // 固定步长 accumulator 在内部分派 0..N 个子步
void     step_once();                // 暂停态单步（编辑器/调试用）

u64      body_create(const body_desc& d);    // 返回 engine body id（从 1 起），0 = 失败
bool     body_remove(u64 id);
bool     body_get_transform(u64 id, body_transform& out);
bool     body_set_transform(u64 id, const math::v3& position, const math::v3& euler); // teleport + 唤醒
bool     body_set_linear_velocity(u64 id, const math::v3& v);
bool     body_apply_impulse(u64 id, const math::v3& impulse);

// ---- 场景查询（N3：PFX IPhysicsQuery 适配后端）----
// 从 from 沿 dir 发射长 max_dist 的射线，取最近命中。命中返回 true 并写点/法线（世界空间）。
bool     raycast(const math::v3& from, const math::v3& dir, f32 max_dist,
                 math::v3& out_point, math::v3& out_normal);

// 全体 body 位姿的稳定哈希（单线程模式下同平台 bit 级确定，回归门禁用）
u64      determinism_hash();

// 可视化：把所有 debug_draw_proxy body 的线框写入 graphics::debug_draw 队列。返回写入线段数。
u32      debug_draw_bodies();
u64      debug_lines_forwarded();            // 累计线段数（测试断言用）

// ---- 碰撞事件（M4-17：ContactListener 收集，步末入队；drain 取走后清空）----
struct contact_event
{
    u64 body_a;                   // 引擎 body id（body_create 返回值）
    u64 body_b;
    f32 point[3];                 // 世界空间接触点（取 manifold 首点）
    u32 added;                    // 1=新增接触（OnContactAdded）0=持续接触
};

// 取走至多 max 条接触事件，返回实际条数。每帧调用一次为宜。
u32  drain_contacts(contact_event* out, u32 max);

// ---- 扩展碰撞体（M4-19：凸包 / 三角网）----
// 凸包：points 为 count 个世界空间点（xyz 连续），由 Jolt 生成凸包壳
// 三角网：仅支持静态体（Jolt MeshShape 不可动）；顶点 + 三角索引
u64  body_create_convex(const body_desc& d, const math::v3* points, u32 count);

// ---- 关节（physics S2，2026-10-15：铰链 + 限位 + 驱动）----
struct joint_desc
{
    enum class kind : u32 { hinge = 0 };

    kind       k{ kind::hinge };
    u64        body_a{};              // 引擎 body id（body_create 返回；0=世界固定）
    u64        body_b{};              // 从动体（动态）
    math::v3   world_anchor{};        // 世界空间锚点（米）
    math::v3   axis{};                // 旋转轴（世界空间，自动归一化）
    f32        limit_min_deg{};       // 限位（度；min>=max = 不限位）
    f32        limit_max_deg{};
    bool       motor{};               // 驱动（角速度目标）
    f32        motor_speed_deg_sec{}; // 目标角速度
    f32        motor_torque{};        // 最大扭矩（N·m；0 = 无驱动效果）
};

/// <summary>创建关节。返回引擎关节 id（1 起；0=失败——未初始化/刚体不存在/
/// 轴退化）。限位语义：limit_min_deg < limit_max_deg 才启用；motor 需要
/// torque > 0。</summary>
u64  joint_create(const joint_desc& d);
bool joint_remove(u64 joint_id);

/// <summary>关节状态查询（诊断/验收）：out_angle_deg = 相对初始位形的角度；
/// out_anchor_separation = 两锚点世界距离（米——约束误差的直接度量）。
/// 返回 false = 关节不存在。</summary>
bool joint_get_state(u64 joint_id, f32& out_angle_deg, f32& out_anchor_separation);
u64  body_create_mesh(const body_desc& d, const math::v3* vertices, u32 vcount,
                      const u32* indices, u32 icount);

} // namespace primal::simulation
