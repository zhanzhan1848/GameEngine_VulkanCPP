#include "PhysicsWorld.h"
#include "ShapeFactory.h"
#include "SimulationLayers.h"
#include "SimulationDebugDraw.h"
#include "BodyEntityMap.h"

#include "Common/CoreLogStream.h"
#include "Components/Transform.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Core/TempAllocator.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace primal::simulation {

// ---------------------------------------------------------------------------
// fixed_step_accumulator（纯逻辑；实现在此以便单测链接 Engine 复用）
// ---------------------------------------------------------------------------

u32 fixed_step_accumulator::advance(f32 host_dt)
{
    if (host_dt <= 0.f) return 0;
    // 钳制单帧步长，避免卡顿后的"死亡螺旋"（补帧耗时 → 更大 dt → 更多补帧）
    accumulator += (host_dt > 0.25f) ? 0.25f : host_dt;

    u32 steps{ 0 };
    while (accumulator >= fixed_dt && steps < max_sub_steps) {
        accumulator -= fixed_dt;
        ++steps;
    }
    if (steps == max_sub_steps) {
        // 连续追不上：丢弃余量，不让误差跨帧累积（行为可预期）
        accumulator = 0.f;
    }
    return steps;
}

// ---------------------------------------------------------------------------
// 全局单例状态
// ---------------------------------------------------------------------------

namespace {

struct world_state
{
    settings                        cfg{};
    bool                            initialized{ false };
    bool                            running{ false };
    fixed_step_accumulator          accumulator{};

    std::unique_ptr<JPH::PhysicsSystem>             system;
    std::unique_ptr<JPH::TempAllocatorMalloc>       temp;       // Update() 的帧内临时分配
    std::unique_ptr<JPH::JobSystemSingleThreaded>   jobs_single;
    std::unique_ptr<JPH::JobSystemThreadPool>       jobs_mt;    // M2-lite:max_worker_threads>0 时启用
    JPH::JobSystem*                                 jobs{ nullptr };  // Update 用的当前实现
    std::unique_ptr<layers::jolt_tables>            tables;
#ifdef JPH_DEBUG_RENDERER
    std::unique_ptr<simulation_debug_renderer>      debug_renderer;
#endif
    body_entity_map                                 bodies;
    // physics S2：关节表（引擎关节 id → Jolt ConstraintID + 端点 body id；
    // body_remove 时连带销毁防悬垂）
    struct joint_record
    {
        JPH::Constraint* cid;
        u64 a; u64 b;
        JPH::Vec3 local_a;   // 锚点在两 body 本地空间（创建时算）——误差度量用
        JPH::Vec3 local_b;
    };
    std::unordered_map<u64, joint_record>        joints;
    u64                                          next_joint_id{ 1 };
    // physics S2：角色表（意图驱动 + 卡住诊断）
    struct character_record
    {
        u64        body{};
        f32        speed{ 4.f };
        f32        half_height{ 0.9f };
        math::v3   intent{};            // 归一化意图（0=停）
        math::v3   last_probe_pos{};    // 卡住检测采样点
        f32        probe_timer{ 0.f };  // 距上次采样秒数
        bool       stuck{ false };
    };
    std::unordered_map<u64, character_record>   characters;
    u64                                          next_character_id{ 1 };
};

world_state& state()
{
    static world_state s;
    return s;
}

// ---- 碰撞事件收集（M4-17）----
// ContactListener 回调在多线程下触发（Jolt 文档），队列用互斥保护；
// drain 在主线程（step 末尾由调用方执行）。
struct contact_queue
{
    std::mutex mutex;
    std::vector<simulation::contact_event> pending;
};

contact_queue& contacts()
{
    static contact_queue q;
    return q;
}

class contact_listener_impl final : public JPH::ContactListener
{
public:
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        record(b1, b2, m, 1);
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2,
                            const JPH::ContactManifold& m, JPH::ContactSettings&) override
    {
        record(b1, b2, m, 0);
    }

private:
    static void record(const JPH::Body& b1, const JPH::Body& b2,
                       const JPH::ContactManifold& m, u8 added)
    {
        simulation::contact_event ev{};
        ev.body_a = b1.GetUserData();
        ev.body_b = b2.GetUserData();
        if (!m.mRelativeContactPointsOn1.empty())
        {
            const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
            ev.point[0] = static_cast<f32>(p.GetX());
            ev.point[1] = static_cast<f32>(p.GetY());
            ev.point[2] = static_cast<f32>(p.GetZ());
        }
        ev.added = added;
        std::lock_guard<std::mutex> lock(contacts().mutex);
        // 上限保护：无人 drain 时防无界增长（编辑器长跑场景）
        if (contacts().pending.size() > 8192)
            contacts().pending.erase(contacts().pending.begin(), contacts().pending.begin() + 4096);
        contacts().pending.push_back(ev);
    }
};

contact_listener_impl& contact_listener()
{
    static contact_listener_impl l;
    return l;
}

void jolt_trace_handler(const char* inFmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, inFmt);
    std::vsnprintf(buf, sizeof(buf), inFmt, args);
    va_end(args);
    PRIMAL_LOG_STREAM(Info, "Simulation") << "[Jolt] " << buf << std::endl;
}

math::v3 quat_to_euler_xyz(const JPH::Quat& q)
{
    const JPH::Vec3 e = q.GetEulerAngles();
    return math::v3{ e.GetX(), e.GetY(), e.GetZ() };
}

// M1：owner 绑定回写——物理 body 的位姿写回绑定实体的 ECS transform，
// 渲染管线的 SyncEntitiesToRenderScene 由此自动取到（instanced mesh 路径）。
void sync_bound_transforms()
{
    world_state& ws = state();
    const JPH::BodyInterface& bi = ws.system->GetBodyInterfaceNoLock();
    ws.bodies.for_each_ordered([&](u64, const body_entity_map::record& r) {
        if (!r.owner.is_valid()) return;
        const JPH::RVec3 pos = bi.GetPosition(r.jid);
        const JPH::Quat rot = bi.GetRotation(r.jid);
        primal::transform::component tc{ primal::transform::transform_id{ r.owner.get_id() } };
        primal::transform::set_position(tc, math::v3{ static_cast<f32>(pos.GetX()),
                                                      static_cast<f32>(pos.GetY()),
                                                      static_cast<f32>(pos.GetZ()) });
        primal::transform::set_orientation(tc, quat_to_euler_xyz(rot));
    });
}

// FNV-1a 64：按 body id 升序混入 (id, 位置位型, 四元数位型)。
// 单线程模式下同平台同构建 bit 级确定（SimulationLayers 单测与 T1 门禁的前提）。
void hash_mix(u64& h, u64 v)
{
    h ^= v;
    h *= 1099511628211ull;
}

u64 hash_f32_bits(u64 h, f32 v)
{
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    hash_mix(h, bits);
    return h;
}

u64 hash_v3(u64 h, const math::v3& v)
{
    h = hash_f32_bits(h, v.x);
    h = hash_f32_bits(h, v.y);
    h = hash_f32_bits(h, v.z);
    return h;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool initialize(const settings& s)
{
    world_state& ws = state();
    if (ws.initialized) shutdown();

    JPH::RegisterDefaultAllocator();
    JPH::Trace = jolt_trace_handler;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    ws.cfg = s;
    ws.tables = std::make_unique<layers::jolt_tables>();
    ws.system = std::make_unique<JPH::PhysicsSystem>();
    ws.temp = std::make_unique<JPH::TempAllocatorMalloc>();
    // max_bodies 同值给 body pairs / contact constraints（M0 量级足够；M2 调优）
    ws.system->Init(s.max_bodies, 0, s.max_bodies * 2, s.max_bodies,
                    ws.tables->bp_interface, *ws.tables->bp_filter, ws.tables->object_filter);
    ws.system->SetGravity(JPH::Vec3{ s.gravity.x, s.gravity.y, s.gravity.z });
    ws.system->SetContactListener(&contact_listener());

    if (s.max_worker_threads == 0) {
        // M0/M1 确定性基线：单线程。接引擎 JobSystem 的完整适配器仍归 M2。
        ws.jobs_single = std::make_unique<JPH::JobSystemSingleThreaded>(s.max_bodies * 4);
        ws.jobs = ws.jobs_single.get();
    } else {
        // P3.3:Jolt 原生线程池(确定性默认不变——默认 settings 仍为 0 单线程)
        const int threads = (int)std::min<u32>(s.max_worker_threads, 16u);
        ws.jobs_mt = std::make_unique<JPH::JobSystemThreadPool>(s.max_bodies * 4,
                                                                s.max_bodies * 2, threads);
        ws.jobs = ws.jobs_mt.get();
        PRIMAL_LOG_STREAM(Info, "Simulation")
            << "job system: thread pool (" << threads << " workers)" << std::endl;
    }

    ws.accumulator.fixed_dt = s.fixed_dt;
    ws.accumulator.max_sub_steps = s.max_sub_steps;
    ws.accumulator.reset();
    ws.bodies = body_entity_map{};
    ws.running = false;
    ws.initialized = true;

    PRIMAL_LOG_STREAM(Info, "Simulation")
        << "initialized: gravity=(" << s.gravity.x << "," << s.gravity.y << "," << s.gravity.z
        << ") max_bodies=" << s.max_bodies << " fixed_dt=" << s.fixed_dt << std::endl;
    return true;
}

void shutdown()
{
    world_state& ws = state();
    if (!ws.initialized) return;

    // Jolt 顺序：先销毁 PhysicsSystem（其 body 池引用 factory 注册的类型），再拆 factory。
    ws.system.reset();
    ws.jobs_single.reset();
    ws.jobs_mt.reset();
    ws.jobs = nullptr;
    ws.temp.reset();
    ws.tables.reset();
#ifdef JPH_DEBUG_RENDERER
    ws.debug_renderer.reset();
#endif
    ws.bodies = body_entity_map{};

    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    JPH::UnregisterTypes();

    ws.initialized = false;
    PRIMAL_LOG_STREAM(Info, "Simulation") << "shutdown" << std::endl;
}

bool is_initialized() { return state().initialized; }
void set_running(bool r) { state().running = r; }
bool running() { return state().running; }

// ---------------------------------------------------------------------------
// 步进
// ---------------------------------------------------------------------------

// 角色意图应用（step 的 physics 子步前）：水平速度=意图×速度、垂直保留
static void characters_apply_intents(world_state& ws)
{
    for (auto& [id, ch] : ws.characters)
    {
        JPH::BodyID jid{};
        game_entity::entity owner{};
        if (!ws.bodies.get(ch.body, jid, owner)) continue;
        JPH::BodyInterface& bi = ws.system->GetBodyInterface();
        const JPH::Vec3 vel = bi.GetLinearVelocity(jid);
        const JPH::Vec3 target{
            ch.intent.x * ch.speed, vel.GetY(), ch.intent.z * ch.speed };
        bi.SetLinearVelocity(jid, target);
    }
}

// 卡住探测：意图非零时每 0.5s 采样位移；< 期望 30% → stuck（清零意图不改——
// 诊断语义，策略归上层）
static void characters_probe_stuck(world_state& ws, f32 sub_dt)
{
    for (auto& [id, ch] : ws.characters)
    {
        if (ch.probe_timer < 0.5f) { ch.probe_timer += sub_dt; continue; }
        ch.probe_timer = 0.f;
        if (ch.intent.x == 0.f && ch.intent.z == 0.f) { ch.stuck = false; continue; }
        JPH::BodyID jid{};
        game_entity::entity owner{};
        if (!ws.bodies.get(ch.body, jid, owner)) continue;
        const JPH::RVec3 pos = ws.system->GetBodyInterface().GetPosition(jid);
        const f32 dx = (f32)pos.GetX() - ch.last_probe_pos.x;
        const f32 dz = (f32)pos.GetZ() - ch.last_probe_pos.z;
        const f32 moved = sqrtf(dx * dx + dz * dz);
        const f32 expected = ch.speed * 0.5f;
        ch.stuck = moved < expected * 0.3f;
        ch.last_probe_pos = math::v3{ (f32)pos.GetX(), (f32)pos.GetY(), (f32)pos.GetZ() };
    }
}

void step(f32 host_dt)
{
    world_state& ws = state();
    if (!ws.initialized || !ws.running) return;

    const u32 n = ws.accumulator.advance(host_dt);
    for (u32 i = 0; i < n; ++i) {
        characters_apply_intents(ws);   // physics S2：角色意图 → 速度（子步前）
        ws.system->Update(ws.cfg.fixed_dt, 1, ws.temp.get(), ws.jobs);
        characters_probe_stuck(ws, ws.cfg.fixed_dt);
    }
    sync_bound_transforms();
}

void step_once()
{
    world_state& ws = state();
    if (!ws.initialized) return;
    ws.system->Update(ws.cfg.fixed_dt, 1, ws.temp.get(), ws.jobs);
    sync_bound_transforms();
}

// ---------------------------------------------------------------------------
// body 管理
// ---------------------------------------------------------------------------

// 公共物化路径：形状已就绪 → BodyCreationSettings → 入世界 → 注册映射 + user data
u64 create_body_with_shape(const body_desc& d, const JPH::ShapeRefC& shape)
{
    world_state& ws = state();

    JPH::EMotionType motion = JPH::EMotionType::Dynamic;
    JPH::EActivation activation = JPH::EActivation::Activate;
    switch (d.m) {
        case body_desc::mode::static_body:
            motion = JPH::EMotionType::Static;
            activation = JPH::EActivation::DontActivate;
            break;
        case body_desc::mode::kinematic:
            motion = JPH::EMotionType::Kinematic;
            break;
        case body_desc::mode::dynamic:
        default:
            break;
    }

    JPH::BodyCreationSettings bcs{
        shape,
        JPH::RVec3{ d.position.x, d.position.y, d.position.z },
        JPH::Quat::sEulerAngles(JPH::Vec3{ d.orientation_euler.x, d.orientation_euler.y, d.orientation_euler.z }),
        motion,
        static_cast<JPH::ObjectLayer>(d.layer) };

    bcs.mRestitution = d.restitution;
    bcs.mFriction = d.friction;
    if (d.m == body_desc::mode::dynamic) {
        // 由质量反算惯性（密度路径留给 M1 的体积校验）
        bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass = d.mass;
    }

    const JPH::BodyID jid = ws.system->GetBodyInterface().CreateAndAddBody(bcs, activation);
    if (jid.IsInvalid()) {
        PRIMAL_LOG_STREAM(Error, "Simulation") << "body_create: body pool exhausted" << std::endl;
        return 0;
    }
    const u64 engine_id = ws.bodies.add(jid, d.owner, d.s, d.half_extents, d.debug_draw_proxy);
    // user data 反查：ContactListener 回调多线程场景下由此取回引擎 body id
    ws.system->GetBodyInterface().SetUserData(jid, engine_id);
    return engine_id;
}

u64 body_create(const body_desc& d)
{
    world_state& ws = state();
    if (!ws.initialized) return 0;

    JPH::ShapeRefC shape = make_body_shape(d);
    if (shape == nullptr) {
        PRIMAL_LOG_STREAM(Error, "Simulation") << "body_create: unsupported/invalid shape" << std::endl;
        return 0;
    }
    return create_body_with_shape(d, shape);
}

bool body_remove(u64 id)
{
    // physics S2：刚体删除前先销毁其参与关节（Jolt 约束持有 body 引用）
    {
        world_state& ws = state();
        std::vector<u64> doomed;
        for (const auto& [jid, rec] : ws.joints)
            if (rec.a == id || rec.b == id) doomed.push_back(jid);
        for (u64 jid : doomed)
        {
            ws.system->RemoveConstraint(ws.joints[jid].cid);
            ws.joints.erase(jid);
        }
    }

    world_state& ws = state();
    if (!ws.initialized) return false;

    JPH::BodyID jid;
    game_entity::entity owner;
    if (!ws.bodies.get(id, jid, owner)) return false;
    ws.system->GetBodyInterface().RemoveBody(jid);
    ws.system->GetBodyInterface().DestroyBody(jid);
    return ws.bodies.remove(id);
}

bool body_get_transform(u64 id, body_transform& out)
{
    world_state& ws = state();
    if (!ws.initialized) return false;

    JPH::BodyID jid;
    game_entity::entity owner;
    if (!ws.bodies.get(id, jid, owner)) return false;

    const JPH::BodyInterface& bi = ws.system->GetBodyInterfaceNoLock();
    const JPH::RVec3 pos = bi.GetPosition(jid);
    const JPH::Quat rot = bi.GetRotation(jid);
    const JPH::Vec3 vel = bi.GetLinearVelocity(jid);

    out.position = math::v3{ static_cast<f32>(pos.GetX()), static_cast<f32>(pos.GetY()), static_cast<f32>(pos.GetZ()) };
    out.orientation_euler = quat_to_euler_xyz(rot);
    out.linear_velocity = math::v3{ vel.GetX(), vel.GetY(), vel.GetZ() };

    JPH::BodyLockRead lock(ws.system->GetBodyLockInterface(), jid);
    out.sleeping = lock.Succeeded() && !lock.GetBody().IsActive();
    return true;
}

bool body_set_transform(u64 id, const math::v3& position, const math::v3& euler)
{
    world_state& ws = state();
    if (!ws.initialized) return false;

    JPH::BodyID jid;
    game_entity::entity owner;
    if (!ws.bodies.get(id, jid, owner)) return false;

    JPH::BodyInterface& bi = ws.system->GetBodyInterface();
    bi.SetPosition(jid, JPH::RVec3{ position.x, position.y, position.z }, JPH::EActivation::Activate);
    bi.SetRotation(jid,
                   JPH::Quat::sEulerAngles(JPH::Vec3{ euler.x, euler.y, euler.z }),
                   JPH::EActivation::Activate);
    return true;
}

bool body_set_linear_velocity(u64 id, const math::v3& v)
{
    world_state& ws = state();
    if (!ws.initialized) return false;

    JPH::BodyID jid;
    game_entity::entity owner;
    if (!ws.bodies.get(id, jid, owner)) return false;
    ws.system->GetBodyInterface().SetLinearVelocity(jid, JPH::Vec3{ v.x, v.y, v.z });
    return true;
}

bool body_apply_impulse(u64 id, const math::v3& impulse)
{
    world_state& ws = state();
    if (!ws.initialized) return false;

    JPH::BodyID jid;
    game_entity::entity owner;
    if (!ws.bodies.get(id, jid, owner)) return false;
    ws.system->GetBodyInterface().AddImpulse(jid, JPH::Vec3{ impulse.x, impulse.y, impulse.z });
    return true;
}
// ---- 场景查询 --------------------------------------------------------------

bool raycast(const math::v3& from, const math::v3& dir, f32 max_dist,
             math::v3& out_point, math::v3& out_normal)
{
    world_state& ws = state();
    if (!ws.initialized) return false;

    const JPH::Vec3 jdir(dir.x, dir.y, dir.z);
    if (jdir.LengthSq() < 1e-8f || max_dist <= 0.f) return false;

    const JPH::RRayCast ray(JPH::RVec3(from.x, from.y, from.z), jdir * max_dist);
    JPH::RayCastResult hit;
    if (!ws.system->GetNarrowPhaseQueryNoLock().CastRay(ray, hit)) return false;

    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out_point = math::v3{ static_cast<f32>(point.GetX()), static_cast<f32>(point.GetY()),
                          static_cast<f32>(point.GetZ()) };

    JPH::BodyLockRead lock(ws.system->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded() && lock.GetBody().IsRigidBody()) {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
        out_normal = math::v3{ n.GetX(), n.GetY(), n.GetZ() };
    } else {
        out_normal = math::v3{ 0.f, 1.f, 0.f };
    }
    return true;
}


// ---------------------------------------------------------------------------
// 接触事件 drain
// ---------------------------------------------------------------------------

u32 drain_contacts(contact_event* out, u32 max)
{
    if (out == nullptr || max == 0) return 0;
    contact_queue& q = contacts();
    std::lock_guard<std::mutex> lock(q.mutex);
    const u32 n = static_cast<u32>(std::min<size_t>(q.pending.size(), max));
    std::copy(q.pending.begin(), q.pending.begin() + n, out);
    q.pending.erase(q.pending.begin(), q.pending.begin() + n);
    return n;
}

// ---------------------------------------------------------------------------
// 扩展碰撞体（凸包 / 三角网）
// ---------------------------------------------------------------------------

u64 body_create_convex(const body_desc& d, const math::v3* points, u32 count)
{
    world_state& ws = state();
    if (!ws.initialized || points == nullptr || count < 4) return 0;

    JPH::ConvexHullShapeSettings settings;
    settings.mPoints.reserve(count);
    for (u32 i = 0; i < count; ++i)
        settings.mPoints.push_back(JPH::Vec3{ points[i].x, points[i].y, points[i].z });
    const auto result = settings.Create();
    if (result.HasError()) {
        PRIMAL_LOG_STREAM(Error, "Simulation") << "body_create_convex: " << result.GetError().c_str() << std::endl;
        return 0;
    }
    return create_body_with_shape(d, result.Get());
}

u64 body_create_mesh(const body_desc& d, const math::v3* vertices, u32 vcount,
                     const u32* indices, u32 icount)
{
    world_state& ws = state();
    if (!ws.initialized || vertices == nullptr || indices == nullptr) return 0;
    if (d.m != body_desc::mode::static_body) {
        PRIMAL_LOG_STREAM(Error, "Simulation") << "body_create_mesh: MeshShape 仅支持静态体" << std::endl;
        return 0;
    }

    JPH::TriangleList triangles;
    triangles.reserve(icount / 3);
    for (u32 i = 0; i + 2 < icount; i += 3) {
        const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vcount || b >= vcount || c >= vcount) return 0;
        triangles.push_back(JPH::Triangle{
            JPH::Float3{ vertices[a].x, vertices[a].y, vertices[a].z },
            JPH::Float3{ vertices[b].x, vertices[b].y, vertices[b].z },
            JPH::Float3{ vertices[c].x, vertices[c].y, vertices[c].z } });
    }
    JPH::MeshShapeSettings settings{ triangles };
    const auto result = settings.Create();
    if (result.HasError()) {
        PRIMAL_LOG_STREAM(Error, "Simulation") << "body_create_mesh: " << result.GetError().c_str() << std::endl;
        return 0;
    }
    return create_body_with_shape(d, result.Get());
}

// ---------------------------------------------------------------------------
// 确定性哈希与可视化
// ---------------------------------------------------------------------------

u64 determinism_hash()
{
    world_state& ws = state();
    if (!ws.initialized) return 0;

    u64 h = 1469598103934665603ull;
    ws.bodies.for_each_ordered([&](u64 id, const body_entity_map::record&) {
        JPH::BodyID jid;
        game_entity::entity owner;
        if (!ws.bodies.get(id, jid, owner)) return;

        const JPH::BodyInterface& bi = ws.system->GetBodyInterfaceNoLock();
        const JPH::RVec3 pos = bi.GetPosition(jid);
        const JPH::Quat rot = bi.GetRotation(jid);

        hash_mix(h, id);
        h = hash_f32_bits(h, static_cast<f32>(pos.GetX()));
        h = hash_f32_bits(h, static_cast<f32>(pos.GetY()));
        h = hash_f32_bits(h, static_cast<f32>(pos.GetZ()));
        h = hash_f32_bits(h, rot.GetX());
        h = hash_f32_bits(h, rot.GetY());
        h = hash_f32_bits(h, rot.GetZ());
        h = hash_f32_bits(h, rot.GetW());
    });
    return h;
}

u32 debug_draw_bodies()
{
    world_state& ws = state();
#ifdef JPH_DEBUG_RENDERER
    if (!ws.initialized) return 0;
    if (!ws.debug_renderer) ws.debug_renderer = std::make_unique<simulation_debug_renderer>();

    const JPH::uint64 before = ws.debug_renderer->lines_forwarded();
    // 逐体画线框（不走去 DrawGeometry 的形状批次路径——本 renderer 只转发线段）
    ws.bodies.for_each_ordered([&](u64, const body_entity_map::record& r) {
        if (!r.debug_draw_proxy) return;
        JPH::BodyID jid;
        game_entity::entity owner;
        if (!ws.bodies.get(r.id, jid, owner)) return;

        const JPH::BodyInterface& bi = ws.system->GetBodyInterfaceNoLock();
        const JPH::RVec3 pos = bi.GetPosition(jid);
        const JPH::Quat rot = bi.GetRotation(jid);
        const JPH::RMat44 xf = JPH::RMat44::sRotationTranslation(rot, pos);

        if (r.shape == body_desc::shape::sphere) {
            ws.debug_renderer->DrawWireSphere(pos, r.half_extents.x, JPH::Color{ 0, 255, 0, 255 });
        } else {
            const JPH::Vec3 he{ r.half_extents.x, r.half_extents.y, r.half_extents.z };
            const JPH::AABox local{ -he, he };
            ws.debug_renderer->DrawWireBox(xf, local, JPH::Color{ 0, 255, 0, 255 });
        }
    });
    const JPH::uint64 delta = ws.debug_renderer->lines_forwarded() - before;
    return static_cast<u32>(delta > 0xFFFFFFFFull ? 0xFFFFFFFFull : delta);
#else
    (void)ws;
    return 0;
#endif
}

u64 debug_lines_forwarded()
{
    world_state& ws = state();
#ifdef JPH_DEBUG_RENDERER
    return ws.debug_renderer ? ws.debug_renderer->lines_forwarded() : 0;
#else
    (void)ws;
    return 0;
#endif
}


// ---- 关节（physics S2）------------------------------------------------------

u64 joint_create(const joint_desc& d)
{
    world_state& ws = state();
    if (!ws.initialized) return 0;

    JPH::Vec3 axis{ d.axis.x, d.axis.y, d.axis.z };
    const f32 axis_len = axis.Length();
    if (axis_len < 1e-6f) return 0;
    axis = axis / axis_len;

    // S2 合同如实：hinge 需要两个真实 body（body_a=0 世界固定端不支持）
    if (d.body_a == 0 || d.body_b == 0) return 0;

    JPH::BodyID jid_a{}, jid_b{};
    game_entity::entity owner_a{}, owner_b{};
    if (!ws.bodies.get(d.body_a, jid_a, owner_a)) return 0;
    if (!ws.bodies.get(d.body_b, jid_b, owner_b)) return 0;

    JPH::TwoBodyConstraint* c = nullptr;
    JPH::Vec3 local_a{}, local_b{};
    {
        // 锁只包住 Create/锚点计算：AddConstraint 内部自取锁（再持有=锁序断言）
        JPH::BodyLockWrite lock_a(ws.system->GetBodyLockInterfaceNoLock(), jid_a);
        JPH::BodyLockWrite lock_b(ws.system->GetBodyLockInterfaceNoLock(), jid_b);
        if (!lock_a.Succeeded() || !lock_b.Succeeded()) return 0;
        JPH::Body& body_a = lock_a.GetBody();
        JPH::Body& body_b = lock_b.GetBody();

        JPH::HingeConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mPoint1 = JPH::RVec3{ d.world_anchor.x, d.world_anchor.y, d.world_anchor.z };
        settings.mPoint2 = settings.mPoint1;
        settings.mHingeAxis1 = settings.mHingeAxis2 = axis;
        JPH::Vec3 helper = std::fabs(axis.GetY()) < 0.9f ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisX();
        JPH::Vec3 normal = axis.Cross(helper).Normalized();
        settings.mNormalAxis1 = settings.mNormalAxis2 = normal;
        if (d.limit_min_deg < d.limit_max_deg)
        {
            // primal::math::pi 替代 M_PI（MSVC <cmath> 无 POSIX M_PI；常量经 CommonHeaders 传递）
            settings.mLimitsMin = d.limit_min_deg * primal::math::pi / 180.f;
            settings.mLimitsMax = d.limit_max_deg * primal::math::pi / 180.f;
        }
        settings.mMotorSettings.SetTorqueLimit(d.motor_torque);
        c = settings.Create(body_a, body_b);
        if (d.motor && d.motor_torque > 0.f)
        {
            auto* hinge = static_cast<JPH::HingeConstraint*>(c);
            hinge->SetMotorState(JPH::EMotorState::Velocity);
            hinge->SetTargetAngularVelocity(d.motor_speed_deg_sec * primal::math::pi / 180.f);
            // 驱动关节保持动态端点清醒（否则 Jolt 休眠判据会把恒扭矩小运动体
            // 睡掉；静态体断言拒绝 SetAllowSleeping——先判 IsStatic）
            if (!body_a.IsStatic()) body_a.SetAllowSleeping(false);
            if (!body_b.IsStatic()) body_b.SetAllowSleeping(false);
        }

        // 局部锚点：世界锚点经创建时逆变换入各 body 本地（误差度量的正确基准）
        const JPH::RVec3 anchor{ d.world_anchor.x, d.world_anchor.y, d.world_anchor.z };
        local_a = body_a.GetWorldTransform().Inversed3x3() * JPH::Vec3(anchor - body_a.GetPosition());
        local_b = body_b.GetWorldTransform().Inversed3x3() * JPH::Vec3(anchor - body_b.GetPosition());
    }

    ws.system->AddConstraint(c);
    const u64 id = ws.next_joint_id++;
    ws.joints[id] = world_state::joint_record{ c, d.body_a, d.body_b, local_a, local_b };
    return id;
}

bool joint_remove(u64 joint_id)
{
    world_state& ws = state();
    const auto it = ws.joints.find(joint_id);
    if (it == ws.joints.end()) return false;
    ws.system->RemoveConstraint(it->second.cid);
    ws.joints.erase(it);
    return true;
}

bool joint_get_state(u64 joint_id, f32& out_angle_deg, f32& out_anchor_separation)
{
    world_state& ws = state();
    const auto it = ws.joints.find(joint_id);
    if (it == ws.joints.end()) return false;

    JPH::BodyID jid_a{}, jid_b{};
    game_entity::entity owner_a{}, owner_b{};
    if (!ws.bodies.get(it->second.a, jid_a, owner_a)) return false;
    if (!ws.bodies.get(it->second.b, jid_b, owner_b)) return false;

    JPH::BodyLockRead lock_a(ws.system->GetBodyLockInterfaceNoLock(), jid_a);
    JPH::BodyLockRead lock_b(ws.system->GetBodyLockInterfaceNoLock(), jid_b);
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return false;

    auto* hinge = static_cast<JPH::HingeConstraint*>(it->second.cid);
    if (hinge == nullptr) return false;
    out_angle_deg = hinge->GetCurrentAngle() * 180.f / primal::math::pi;

    // 约束误差：锚点经两 body 当前变换回到世界后的距离（局部锚点在创建时算）
    const JPH::RVec3 wa1 = lock_a.GetBody().GetCenterOfMassTransform() * it->second.local_a;
    const JPH::RVec3 wa2 = lock_b.GetBody().GetCenterOfMassTransform() * it->second.local_b;
    const JPH::RVec3 delta = wa1 - wa2;
    out_anchor_separation = std::sqrt(delta.LengthSq());
    return true;
}


// ---- 基础角色移动（physics S2）----------------------------------------------

u64 character_create(const character_desc& d)
{
    world_state& ws = state();
    if (!ws.initialized) return 0;
    if (d.radius <= 0 || d.height <= 0 || d.walk_speed <= 0 || d.mass <= 0) return 0;

    body_desc bd{};
    bd.s = body_desc::shape::box;
    bd.m = body_desc::mode::dynamic;
    bd.half_extents = math::v3{ d.radius, d.height * 0.5f, d.radius };
    bd.position = d.position;
    bd.mass = d.mass;
    bd.friction = 0.2f;           // 低摩擦：意图驱动不被地面阻力吃掉
    bd.owner = d.owner;           // 绑定实体 → step 自动回写 transform
    const u64 body = body_create(bd);
    if (body == 0) return 0;

    world_state::character_record rec{};
    rec.body = body;
    rec.speed = d.walk_speed;
    rec.half_height = d.height * 0.5f;
    rec.last_probe_pos = d.position;
    const u64 id = ws.next_character_id++;
    ws.characters[id] = rec;
    return id;
}

bool character_remove(u64 id)
{
    world_state& ws = state();
    const auto it = ws.characters.find(id);
    if (it == ws.characters.end()) return false;
    const bool ok = body_remove(it->second.body);
    ws.characters.erase(it);
    return ok;
}

bool character_move_intent(u64 id, f32 dir_x, f32 dir_z)
{
    world_state& ws = state();
    const auto it = ws.characters.find(id);
    if (it == ws.characters.end()) return false;
    const f32 len = sqrtf(dir_x * dir_x + dir_z * dir_z);
    if (len < 1e-6f)
    {
        it->second.intent = math::v3{ 0, 0, 0 };
        return true;
    }
    it->second.intent = math::v3{ dir_x / len, 0, dir_z / len };
    return true;
}

bool character_get_state(u64 id, character_state& out)
{
    world_state& ws = state();
    const auto it = ws.characters.find(id);
    if (it == ws.characters.end()) return false;
    const world_state::character_record& ch = it->second;

    JPH::BodyID jid{};
    game_entity::entity owner{};
    if (!ws.bodies.get(ch.body, jid, owner)) return false;
    const JPH::BodyInterface& bi = ws.system->GetBodyInterfaceNoLock();
    const JPH::RVec3 pos = bi.GetPosition(jid);
    const JPH::Vec3 vel = bi.GetLinearVelocity(jid);

    out.position = math::v3{ (f32)pos.GetX(), (f32)pos.GetY(), (f32)pos.GetZ() };
    out.horizontal_speed = sqrtf(vel.GetX() * vel.GetX() + vel.GetZ() * vel.GetZ());
    out.stuck = ch.stuck;
    out.intent = ch.intent;

    // grounded：体心向下短 ray，自体用 BodyFilter 显式排除（自盒 fraction 0
    // 会吃掉单命中 cast——不排除时永远命中自己）
    struct ignore_self final : JPH::BodyFilter
    {
        JPH::BodyID skip;
        bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            return inBodyID != skip;
        }
    };
    out.grounded = false;
    const f32 down = ch.half_height * 1.15f;
    const JPH::RRayCast ray{ pos, JPH::RVec3(0, -down, 0) };
    JPH::RayCastResult hit;
    ignore_self filter{};
    filter.skip = jid;
    if (ws.system->GetNarrowPhaseQueryNoLock().CastRay(ray, hit,
            JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter(), filter))
        out.grounded = true;
    return true;
}

} // namespace
