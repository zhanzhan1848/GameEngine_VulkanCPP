// PhysicsAPI.cpp — 物理模拟（Engine/Simulation × Jolt）的 C ABI 门面，M0 第一批导出。
//
// 设计文档：Docs/2026-09-11-physics-simulation-design.md §5
// 纪律：导出只增不改；POD 镜像用 static_assert 锁尺寸（沿 AssetAPI.h 惯例），
//       C# 侧镜像在 PrimalEditor_Avalonia/Native/PhysicsApi.cs（M2 落地）。
// 单位：米、秒、弧度；右手系 Y 向上（与 Transform 一致）。

#include "Common.h"
#include "CommonHeaders.h"
#include "Simulation/SimulationAPI.h"
#include "Navigation/NavigationRuntime.h"
#include "ParticleFX/core/PfxSystem.h"
#include "ParticleFX/interop/PhysicsQuery.h"
#include "ParticleFX/interop/PhysicsBindings.h"
#include "ParticleFX/host/BudgetManager.h"
#include "Components/Collider.h"
#include "Components/RigidBody.h"
#include "Components/Entity.h"
#include "Components/Script.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "Id.h"

#include <unordered_map>

namespace simulation = primal::simulation;
namespace pfx = primal::pfx;
namespace math = primal::math;
namespace id = primal::id;
namespace game_entity = primal::game_entity;
namespace collider = primal::collider;
namespace rigidbody = primal::rigidbody;

// ---------------------------------------------------------------------------
// POD ABI 镜像（只许用 C 标量数组，不用 math::v3 —— 后者按 SIMD 对齐）
// ---------------------------------------------------------------------------

namespace {

struct PhysicsSettingsPod
{
    f32 gravity[3];
    u32 max_bodies;
    u32 max_worker_threads;   // 0 = 单线程确定性
    f32 fixed_dt;
    u32 max_sub_steps;
};
static_assert(sizeof(PhysicsSettingsPod) == 28,
              "PhysicsSettingsPod ABI must match PrimalEditor_Avalonia mirror (PhysicsApi.cs)");

struct PhysicsBodyDescPod
{
    u32 shape;                // 0=box 1=sphere (simulation::body_desc::shape)
    u32 mode;                 // 0=static 1=dynamic 2=kinematic (simulation::body_desc::mode)
    f32 half_extents[3];      // box 半尺寸 / sphere x=半径
    f32 position[3];
    f32 orientation_euler[3]; // 弧度 XYZ
    f32 mass;
    f32 restitution;
    f32 friction;
    u32 layer;                // simulation::layers 常量
    u32 debug_draw_proxy;     // 0/1
};
static_assert(sizeof(PhysicsBodyDescPod) == 64,
              "PhysicsBodyDescPod ABI must match PrimalEditor_Avalonia mirror (PhysicsApi.cs)");

struct PhysicsBodyTransformPod
{
    f32 position[3];
    f32 orientation_euler[3];
    f32 linear_velocity[3];
    u32 sleeping;             // 0/1
};
static_assert(sizeof(PhysicsBodyTransformPod) == 40,
              "PhysicsBodyTransformPod ABI must match PrimalEditor_Avalonia mirror (PhysicsApi.cs)");

simulation::settings to_settings(const PhysicsSettingsPod* p)
{
    simulation::settings s{};
    s.gravity = math::v3{ p->gravity[0], p->gravity[1], p->gravity[2] };
    s.max_bodies = p->max_bodies;
    s.max_worker_threads = p->max_worker_threads;
    s.fixed_dt = p->fixed_dt;
    s.max_sub_steps = p->max_sub_steps;
    return s;
}

simulation::body_desc to_body_desc(const PhysicsBodyDescPod* p)
{
    simulation::body_desc d{};
    d.s = static_cast<simulation::body_desc::shape>(p->shape);
    d.m = static_cast<simulation::body_desc::mode>(p->mode);
    d.half_extents = math::v3{ p->half_extents[0], p->half_extents[1], p->half_extents[2] };
    d.position = math::v3{ p->position[0], p->position[1], p->position[2] };
    d.orientation_euler = math::v3{ p->orientation_euler[0], p->orientation_euler[1], p->orientation_euler[2] };
    d.mass = p->mass;
    d.restitution = p->restitution;
    d.friction = p->friction;
    d.layer = p->layer;
    d.debug_draw_proxy = p->debug_draw_proxy != 0;
    return d;
}

// --- 组件物化（Collider/RigidBody → 物理 body）--------------------------------
// owner 绑定后由 simulation::step 自动回写 ECS transform。实体 id → body id 映射
// 由本层持有；实体销毁时须先调 RemoveEntityCollider/RemoveEntityRigidBody 反注册
//（M3 的场景同步器负责，M2 由调用方保证）。
// 键 = id::index(entity_id)（槽位索引，无代数位）——MaterializeAll（遍历索引）
// 与 C# 查询（完整 id）两条路径必须一致
std::unordered_map<id::id_type, u64> g_materialized_bodies;

game_entity::entity entity_from_raw(id::id_type id)
{
    return game_entity::entity{ game_entity::entity_id{ id } };
}

bool entity_has_component_raw(id::id_type entity, u8 bit)
{
    return game_entity::has_component_bit(game_entity::entity_id{ entity }, bit);
}

u64 materialize_body(id::id_type entity_id, game_entity::entity entity,
                     u32 shape, u32 mode, const math::v3& half,
                     f32 mass, f32 restitution, f32 friction,
                     u32 layer, const math::v3& velocity)
{
    simulation::body_desc d{};
    d.s = static_cast<simulation::body_desc::shape>(shape);
    d.m = static_cast<simulation::body_desc::mode>(mode);
    d.half_extents = half;
    // 初始位姿取实体当前 transform（M2 简化：offset 不参与，M3 补）
    if (entity.is_valid()) {
        d.owner = entity;
        d.position = entity.position();
    }
    d.mass = mass;
    d.restitution = restitution;
    d.friction = friction;
    d.layer = layer;
    const u64 body_id = simulation::body_create(d);
    if (body_id != 0) {
        g_materialized_bodies[id::index(entity_id)] = body_id;
        if (velocity.x != 0.f || velocity.y != 0.f || velocity.z != 0.f) {
            simulation::body_set_linear_velocity(body_id, velocity);
        }
    }
    return body_id;
}

void dematerialize_body(id::id_type entity_id)
{
    auto it = g_materialized_bodies.find(id::index(entity_id));
    if (it == g_materialized_bodies.end()) return;
    simulation::body_remove(it->second);
    g_materialized_bodies.erase(it);
}

} // anonymous namespace

extern "C" {

// --- 生命周期 -----------------------------------------------------------

EDITOR_INTERFACE u32 PhysicsInitialize(const PhysicsSettingsPod* in_settings)
{
    if (!in_settings) return 0;
    // PFX 碰撞模块（PhysicsCollision）经 physics_query 门面查世界；适配器只装一次。
    pfx::interop::install_simulation_adapter();
    // P2.5:刚体化粒子全局预算与 budget_config 单一口径
    pfx::interop::physics_bindings::get().set_global_body_budget(
        pfx::host::budget_config{}.max_live_rigid_bodies);
    return simulation::initialize(to_settings(in_settings)) ? 1u : 0u;
}

EDITOR_INTERFACE void PhysicsShutdown(void)
{
    pfx::interop::physics_bindings::get().clear();   // 刚体化粒子的绑定连带处置
    simulation::shutdown();
}

EDITOR_INTERFACE u32 PhysicsSetRunning(u32 running)
{
    if (!simulation::is_initialized()) return 0;
    simulation::set_running(running != 0);
    return 1;
}

// 宿主每帧在渲染前调用（PrimalPlayer / 编辑器视口 / 测试）。
// 帧内顺序契约（scripting S1 冻结 + navigation S1 扩展）：script → nav →
// physics → pfx → bindings——脚本先产出意图（entity.set_position/rotation 写
// 变换、nav.move_to 建任务、事件发射），导航随后推进并把步进结果回写实体
// transform，物理步进与粒子消费其后；到期回收产生的 death 事件下帧被消费者
// 拉取。nav 段无任务时零成本；navagent 实体不应再挂 dynamic rigidbody
//（两个驱动源写同一 transform，编辑器侧约束）。
// script 段有 is_initialized 守卫：编辑器进程无脚本组件时从不调
// ScriptEnsureSystems，而 frame_tick 入口的 check_main_thread 断言
// g_initialized——无守卫时编辑器视口首帧即 SIGABRT（2026-10-12 实测）。
EDITOR_INTERFACE void PipelineTick(f32 dt_seconds)
{
    if (primal::script::is_initialized())
        primal::script::frame_tick(dt_seconds);
    primal::navigation::runtime::tick(dt_seconds);
    simulation::step(dt_seconds);
    pfx::tick(dt_seconds);
    // P2:逐粒子刚体绑定——drain 事件一次、路由、物理 transform 回写
    pfx::interop::physics_bindings_tick();
}

EDITOR_INTERFACE void PhysicsStepOnce(void)
{
    simulation::step_once();
}

// --- body 管理 -----------------------------------------------------------

EDITOR_INTERFACE u64 PhysicsBodyCreate(const PhysicsBodyDescPod* in_desc)
{
    if (!in_desc) return 0;
    return simulation::body_create(to_body_desc(in_desc));
}

EDITOR_INTERFACE u32 PhysicsBodyRemove(u64 body_id)
{
    return simulation::body_remove(body_id) ? 1u : 0u;
}

EDITOR_INTERFACE u32 PhysicsBodySetTransform(u64 body_id, const f32* position, const f32* euler)
{
    if (!position || !euler) return 0;
    if (!simulation::is_initialized()) return 0;

    // teleport 语义：瞬移并唤醒（velocity 保留）。组件联动的回写路径不经过这里。
    return simulation::body_set_transform(
               body_id,
               math::v3{ position[0], position[1], position[2] },
               math::v3{ euler[0], euler[1], euler[2] }) ? 1u : 0u;
}

EDITOR_INTERFACE u32 PhysicsBodySetLinearVelocity(u64 body_id, const f32* velocity)
{
    if (!velocity) return 0;
    return simulation::body_set_linear_velocity(
               body_id, math::v3{ velocity[0], velocity[1], velocity[2] }) ? 1u : 0u;
}

EDITOR_INTERFACE u32 PhysicsBodyApplyImpulse(u64 body_id, const f32* impulse)
{
    if (!impulse) return 0;
    return simulation::body_apply_impulse(
               body_id, math::v3{ impulse[0], impulse[1], impulse[2] }) ? 1u : 0u;
}

EDITOR_INTERFACE u32 PhysicsBodyGetTransform(u64 body_id, PhysicsBodyTransformPod* out)
{
    if (!out) return 0;
    simulation::body_transform t{};
    if (!simulation::body_get_transform(body_id, t)) return 0;
    out->position[0] = t.position.x;  out->position[1] = t.position.y;  out->position[2] = t.position.z;
    out->orientation_euler[0] = t.orientation_euler.x;
    out->orientation_euler[1] = t.orientation_euler.y;
    out->orientation_euler[2] = t.orientation_euler.z;
    out->linear_velocity[0] = t.linear_velocity.x;
    out->linear_velocity[1] = t.linear_velocity.y;
    out->linear_velocity[2] = t.linear_velocity.z;
    out->sleeping = t.sleeping ? 1u : 0u;
    return 1;
}

EDITOR_INTERFACE u32 PhysicsBodyIsSleeping(u64 body_id)
{
    simulation::body_transform t{};
    if (!simulation::body_get_transform(body_id, t)) return 0;
    return t.sleeping ? 1u : 0u;
}

// --- 门禁 / 可视化 ---------------------------------------------------------

EDITOR_INTERFACE u64 PhysicsDeterminismHash(void)
{
    return simulation::determinism_hash();
}

EDITOR_INTERFACE u32 PhysicsDebugDrawBodies(void)
{
    return simulation::debug_draw_bodies();
}

// --- M4：接触事件 + 扩展碰撞体 ----------------------------------------------
// contact_event POD 镜像（SimulationAPI.h：body_a u64 / body_b u64 / point[3] f32 / added u32 = 32B）
struct PhysicsContactEventPod
{
    u64 body_a, body_b;
    f32 point[3];
    u32 added;
};
static_assert(sizeof(PhysicsContactEventPod) == 32,
              "PhysicsContactEventPod ABI mismatch vs SimulationAPI.h contact_event");

EDITOR_INTERFACE u32 PhysicsDrainContacts(PhysicsContactEventPod* out, u32 max_count)
{
    if (!out || max_count == 0) return 0;
    return simulation::drain_contacts(reinterpret_cast<simulation::contact_event*>(out), max_count);
}

EDITOR_INTERFACE u64 PhysicsBodyCreateConvex(PhysicsBodyDescPod* desc, const f32* points, u32 point_count)
{
    if (!desc || !points || point_count < 4) return 0;
    simulation::body_desc d = to_body_desc(desc);
    return simulation::body_create_convex(d, reinterpret_cast<const math::v3*>(points), point_count);
}

EDITOR_INTERFACE u64 PhysicsBodyCreateMesh(PhysicsBodyDescPod* desc, const f32* vertices, u32 vertex_count,
                                            const u32* indices, u32 index_count)
{
    if (!desc || !vertices || !indices) return 0;
    simulation::body_desc d = to_body_desc(desc);
    return simulation::body_create_mesh(d, reinterpret_cast<const math::v3*>(vertices), vertex_count,
                                        indices, index_count);
}

// --- 组件导出（M2-D）---------------------------------------------------------
// 坐标/单位：米、弧度，右手系 Y 上。失败返回 0；成功返回 1（body id 经查询接口取）。

EDITOR_INTERFACE u32 AddEntityCollider(id::id_type entity_id, u32 shape,
                                       const f32* half_extents, const f32* offset,
                                       u32 is_trigger, u32 layer)
{
    if (!half_extents || !offset || !primal::id::is_valid(entity_id)) return 0;
    game_entity::entity e{ game_entity::entity_id{ entity_id } };
    if (!e.is_valid() || e.Has<primal::component::Collider>()) return 0;

    collider::init_info info{};
    info.shape = shape;
    info.half_extents = math::v3{ half_extents[0], half_extents[1], half_extents[2] };
    info.offset = math::v3{ offset[0], offset[1], offset[2] };
    info.is_trigger = is_trigger != 0;
    info.layer = layer;
    e.Add<primal::component::Collider>(info);

    // 无 RigidBody 的 Collider = 静态体，立即物化（编辑态即有碰撞；M3 引入 play 态后调整）
    if (!e.Has<primal::component::RigidBody>() && simulation::is_initialized()) {
        const collider::component cc{ collider::collider_component_id{ entity_id } };
        materialize_body(entity_id, e, collider::shape(cc), 0 /*static*/,
                         collider::half_extents(cc), 1.f, 0.05f, 0.5f, collider::layer(cc),
                         math::v3{});
    }
    return 1;
}

EDITOR_INTERFACE u32 RemoveEntityCollider(id::id_type entity_id)
{
    if (!primal::id::is_valid(entity_id)) return 0;
    game_entity::entity e{ game_entity::entity_id{ entity_id } };
    if (!e.is_valid() || !e.Has<primal::component::Collider>()) return 0;

    e.Remove<primal::component::Collider>();
    dematerialize_body(entity_id);
    return 1;
}

EDITOR_INTERFACE u32 AddEntityRigidBody(id::id_type entity_id, u32 mode, f32 mass,
                                        f32 restitution, f32 friction, const f32* velocity)
{
    if (!velocity || !primal::id::is_valid(entity_id)) return 0;
    game_entity::entity e{ game_entity::entity_id{ entity_id } };
    if (!e.is_valid() || e.Has<primal::component::RigidBody>()) return 0;
    if (!e.Has<primal::component::Collider>()) return 0;   // RigidBody 依赖 Collider

    rigidbody::init_info info{};
    info.mode = mode;
    info.mass = mass;
    info.restitution = restitution;
    info.friction = friction;
    info.linear_velocity = math::v3{ velocity[0], velocity[1], velocity[2] };
    e.Add<primal::component::RigidBody>(info);

    if (simulation::is_initialized()) {
        const collider::component cc{ collider::collider_component_id{ entity_id } };
        const rigidbody::component rc{ rigidbody::rigidbody_component_id{ entity_id } };
        dematerialize_body(entity_id);   // 静态体（若有）升级为动态体
        materialize_body(entity_id, e, collider::shape(cc), rigidbody::mode(rc),
                         collider::half_extents(cc), rigidbody::mass(rc),
                         rigidbody::restitution(rc), rigidbody::friction(rc),
                         collider::layer(cc), rigidbody::linear_velocity(rc));
    }
    return 1;
}

EDITOR_INTERFACE u32 RemoveEntityRigidBody(id::id_type entity_id)
{
    if (!primal::id::is_valid(entity_id)) return 0;
    game_entity::entity e{ game_entity::entity_id{ entity_id } };
    if (!e.is_valid() || !e.Has<primal::component::RigidBody>()) return 0;

    e.Remove<primal::component::RigidBody>();
    dematerialize_body(entity_id);
    return 1;
}

EDITOR_INTERFACE u32 EntityHasCollider(id::id_type entity_id)
{
    return entity_has_component_raw(entity_id, static_cast<u8>(primal::component_bit::Collider)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 EntityHasRigidBody(id::id_type entity_id)
{
    return entity_has_component_raw(entity_id, static_cast<u8>(primal::component_bit::RigidBody)) ? 1u : 0u;
}

EDITOR_INTERFACE u64 PhysicsEntityBody(id::id_type entity_id)
{
    auto it = g_materialized_bodies.find(id::index(entity_id));
    return it != g_materialized_bodies.end() ? it->second : 0ull;
}

// 补物化：遍历所有存活原生实体，对带 Collider 且尚未物化的补建物理体
//（sim.play 时调用——sim.body.add 可能发生在物理世界初始化之前）。
// 已物化的实体跳过（幂等）；RigidBody 无 Collider 的实体跳过（依赖前置）。
EDITOR_INTERFACE u32 PhysicsMaterializeAll(void)
{
    if (!simulation::is_initialized()) return 0;
    u32 created = 0;
    const u32 count = game_entity::entity_count();
    for (u32 i = 0; i < count; ++i)
    {
        const game_entity::entity_id eid = game_entity::entity_id_from_index(i);
        if (!game_entity::is_alive(eid)) continue;
        if (!entity_has_component_raw(i, static_cast<u8>(primal::component_bit::Collider))) continue;
        if (g_materialized_bodies.count(i)) continue;

        game_entity::entity e{ eid };
        const collider::component cc{ collider::collider_component_id{ i } };
        const bool has_rb = entity_has_component_raw(i, static_cast<u8>(primal::component_bit::RigidBody));
        const rigidbody::component rc{ rigidbody::rigidbody_component_id{ i } };

        simulation::body_desc d{};
        d.s = static_cast<simulation::body_desc::shape>(collider::shape(cc));
        d.m = has_rb ? static_cast<simulation::body_desc::mode>(rigidbody::mode(rc))
                     : simulation::body_desc::mode::static_body;
        d.half_extents = collider::half_extents(cc);
        d.position = e.position();
        d.layer = collider::layer(cc);
        d.restitution = has_rb ? rigidbody::restitution(rc) : 0.05f;
        d.friction = has_rb ? rigidbody::friction(rc) : 0.5f;
        d.mass = has_rb ? rigidbody::mass(rc) : 1.f;
        d.owner = e;

        const u64 body_id = simulation::body_create(d);
        if (body_id != 0)
        {
            g_materialized_bodies[id::index(i)] = body_id;
            ++created;
        }
    }
    return created;
}

EDITOR_INTERFACE u32 PhysicsIsRunning(void)
{
    return simulation::running() ? 1u : 0u;
}

EDITOR_INTERFACE u32 PhysicsIsInitialized(void)
{
    return simulation::is_initialized() ? 1u : 0u;
}

} // extern "C"
