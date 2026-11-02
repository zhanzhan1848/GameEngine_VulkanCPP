#pragma once

// ParticleFXN2Tests.h — PFX N2 门禁测试（渲染桥，架构文档 §7-N2）。
// 覆盖：sprite 兼容桥（属性表 → 旧 particle_data 交错布局，仅活行、行序稳定）、
//       mesh 实例桥（48B 布局、单位四元数、position/scale 落位）。

#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/ParticleFX/render/Bridges.h"
#include "Engine/Particles/ParticleTypes.h"

#include <cassert>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

inline void test_sprite_bridge_pack() {
    std::cout << "Testing sprite compat bridge packing..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":64,"stages":{
            "spawn":[{"module":"SpawnRate","rate":5}],
            "init":[{"module":"SetLifetime","min":10,"max":10}]}})", 21);
    assert(id != invalid_emitter);
    emitter_get(id)->tick(1.0f);   // 5 粒

    particles::particle_data packed[8]{};
    const u32 n = render::pack_sprite_instances(emitter_get(id)->store(), packed, sizeof(packed));
    assert(n == 5);

    // 行序 = row 升序（首行即 row 0）；字段与 store 一致
    const math::v4* pos = emitter_get(id)->store().v4_channel(attr::position);
    const math::v4* scl = emitter_get(id)->store().v4_channel(attr::scale_rotation);
    for (u32 i = 0; i < n; ++i) {
        assert(packed[i].position.x == pos[i].x && packed[i].position.w == pos[i].w);
        assert(packed[i].scale_rotation.x == scl[i].x);
    }

    // 杀一行（KillAge 语义模拟：直接 free row 0）后打包只剩 4 行且跳过死行
    emitter_get(id)->store().free(emitter_get(id)->store().row_handle(0));
    particles::particle_data packed2[8]{};
    const u32 n2 = render::pack_sprite_instances(emitter_get(id)->store(), packed2, sizeof(packed2));
    assert(n2 == 4);
    assert(packed2[0].position.x == pos[1].x);   // 死行被跳过（压缩打包）

    // 探测用法：null 缓冲返回需要的行数
    assert(render::pack_sprite_instances(emitter_get(id)->store(), nullptr, 0) == 4);

    shutdown();
    std::cout << "  PASS: Sprite compat bridge" << std::endl;
}

inline void test_mesh_bridge_pack() {
    std::cout << "Testing mesh instance bridge packing..." << std::endl;

    static_assert(sizeof(render::mesh_instance_data) == 48, "48B 实例布局锁定");
    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":64,"stages":{
            "spawn":[{"module":"SpawnRate","rate":3}],
            "init":[{"module":"SetLifetime","min":10,"max":10},
                    {"module":"SetScale","scale":[2,3]}]}})", 31);
    emitter_get(id)->tick(1.0f);   // 3 粒

    render::mesh_instance_data inst[4]{};
    const u32 n = render::pack_mesh_instances(emitter_get(id)->store(), inst, 4);
    assert(n == 3);

    const math::v4* pos = emitter_get(id)->store().v4_channel(attr::position);
    for (u32 i = 0; i < n; ++i) {
        assert(inst[i].pos[0] == pos[i].x && inst[i].pos[2] == pos[i].z);
        assert(inst[i].rot[0] == 0.f && inst[i].rot[3] == 1.f);   // 单位四元数
        assert(inst[i].scale[0] == 2.f && inst[i].scale[1] == 3.f);
    }

    shutdown();
    std::cout << "  PASS: Mesh instance bridge" << std::endl;
}

inline void test_volume_bridge_pack() {
    attribute_store store;
    assert(render::pack_volume_particles(store,2,10).empty());
    assert(store.initialize(4,nullptr,0));
    auto dead=store.allocate();
    auto live=store.allocate();
    *static_cast<math::v4*>(store.resolve(live,attr::position))={-2,3,4,0};
    *static_cast<math::v4*>(store.resolve(live,attr::scale_rotation))={-1.5f,1,0,0};
    *static_cast<math::v4*>(store.resolve(live,attr::color))={1,1,1,.25f};
    store.free(dead);
    auto particles=render::pack_volume_particles(store,2,10);
    assert(particles.size()==1 && particles[0].x==-2 && particles[0].z==4);
    assert(particles[0].radius==3 && particles[0].density==2.5f);
    store.free(live);
    assert(render::pack_volume_particles(store,2,10).empty());
    std::cout << "  PASS: Volume bridge live rows, radius, density, removal" << std::endl;
}

inline void run_all_n2_tests() {
    test_sprite_bridge_pack();
    test_mesh_bridge_pack();
    test_volume_bridge_pack();
    std::cout << "All ParticleFX N2 tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
