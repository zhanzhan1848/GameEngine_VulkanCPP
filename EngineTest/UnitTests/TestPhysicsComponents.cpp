// TestPhysicsComponents.cpp — M2-D 组件闭环单测（无 GPU）。
//
// 验证链路：game_entity + Collider/RigidBody 组件 → EngineDLL 层同款物化语义
//（AddEntityCollider/AddEntityRigidBody 的引擎内等价调用）→ simulation::step
// owner 回写 → 实体 transform 被物理驱动。
//
// 注：本测试走引擎内 API（非 EngineDLL），物化逻辑与 PhysicsAPI.cpp 共用
// simulation 门面；DLL 参数镜像的 ABI 锁由 PhysicsAPI.cpp 的 static_assert 承担。

#include "CommonHeaders.h"
#include "Components/Collider.h"
#include "Components/RigidBody.h"
#include "Components/Transform.h"
#include "Components/Entity.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "Simulation/SimulationAPI.h"

#include <cmath>
#include <iostream>

using namespace primal;
namespace simulation = primal::simulation;
namespace math = primal::math;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestPhysicsComponents] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestPhysicsComponents] PASS: " << name << std::endl;
    }
}

game_entity::entity make_entity(const math::v3& pos)
{
    transform::init_info tf{};
    tf.position[0] = pos.x; tf.position[1] = pos.y; tf.position[2] = pos.z;
    tf.rotation[0] = 0.f; tf.rotation[1] = 0.f; tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;
    tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;
    game_entity::entity_info info{};
    info.transform = &tf;
    return game_entity::create(info);
}

} // anonymous namespace

int main()
{
    std::cout << "[TestPhysicsComponents] === begin ===" << std::endl;

    // 物理世界
    simulation::settings phys{};
    phys.gravity = math::v3{ 0.f, -9.81f, 0.f };
    check(simulation::initialize(phys), "physics initialize");
    simulation::set_running(true);

    // 1. 静态体：地面（Collider 无 RigidBody）
    game_entity::entity ground = make_entity(math::v3{ 0.f, -0.5f, 0.f });
    {
        collider::init_info ci{};
        ci.shape = 0;  // box
        ci.half_extents = math::v3{ 10.f, 0.5f, 10.f };
        ci.layer = 0;  // kStatic
        ground.Add<component::Collider>(ci);
    }
    check(ground.Has<component::Collider>(), "ground has collider");
    // 静态体物化（AddEntityCollider 语义：无 RigidBody 的 Collider = 静态体）
    {
        const auto cc = ground.Get<component::Collider>();
        simulation::body_desc g{};
        g.s = static_cast<simulation::body_desc::shape>(collider::shape(cc));
        g.m = simulation::body_desc::mode::static_body;
        g.half_extents = collider::half_extents(cc);
        g.position = ground.position();
        g.layer = collider::layer(cc);
        g.restitution = 0.05f;
        check(simulation::body_create(g) != 0, "static ground body materialized");
    }

    // 2. 动态体：盒（Collider + RigidBody，owner 回写链路）
    game_entity::entity box = make_entity(math::v3{ 0.f, 3.f, 0.f });
    {
        collider::init_info ci{};
        ci.shape = 0;
        ci.half_extents = math::v3{ 0.5f, 0.5f, 0.5f };
        box.Add<component::Collider>(ci);

        rigidbody::init_info ri{};
        ri.mode = 1;   // dynamic
        ri.mass = 1.f;
        ri.restitution = 0.1f;
        ri.friction = 0.5f;
        box.Add<component::RigidBody>(ri);
    }
    check(box.Has<component::Collider>() && box.Has<component::RigidBody>(),
          "box has collider + rigidbody");

    // 组件物化（与 EngineDLL AddEntity* 相同的语义：动态体 + owner 绑定）
    {
        const auto cc = box.Get<component::Collider>();
        const auto rc = box.Get<component::RigidBody>();
        simulation::body_desc d{};
        d.s = static_cast<simulation::body_desc::shape>(collider::shape(cc));
        d.m = simulation::body_desc::mode::dynamic;
        d.half_extents = collider::half_extents(cc);
        d.position = box.position();
        d.mass = rigidbody::mass(rc);
        d.restitution = rigidbody::restitution(rc);
        d.friction = rigidbody::friction(rc);
        d.layer = collider::layer(cc);
        d.owner = box;
        const u64 body = simulation::body_create(d);
        check(body != 0, "dynamic body materialized from components");
    }

    // 3. 固定步长推进：物理 → owner 回写 → 实体 transform 跟随
    f32 min_y = 1e9f;
    for (int i = 0; i < 120; ++i) {
        simulation::step(1.f / 60.f);
        const math::v3 pos = box.position();
        if (pos.y < min_y) min_y = pos.y;
    }
    check(std::fabs(min_y - 0.5f) < 0.2f, "entity transform driven by physics (box landed)");
    std::cout << "[TestPhysicsComponents] box min_y = " << min_y << std::endl;

    std::cout << "[TestPhysicsComponents] === end, failures=" << failures << " ===" << std::endl;
    simulation::shutdown();
    return failures ? 1 : 0;
}
