#pragma once

// ParticleFXTests.h — PFX N0 门禁测试（架构文档 §7 N0）。
// 覆盖：句柄 ABA、事件 FIFO 与溢出丢弃、发射器生命周期事件序、
//       旧 particle_data 布局兼容桥、pcg32 确定性、系统级生命周期。
// 风格沿 Particles 测试惯例：inline 函数 + assert + stdout PASS 行。

#include "Engine/ParticleFX/core/AttributeStore.h"
#include "Engine/ParticleFX/core/EventBus.h"
#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/Particles/ParticleTypes.h"   // 兼容桥对照（测试层持有，core 零依赖）

#include <cassert>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

// ---- attribute_store --------------------------------------------------------

inline void test_store_channels_and_resolve() {
    std::cout << "Testing attribute store channels and resolve..." << std::endl;

    attribute_store store;
    const attr_desc users[] = { { 0x11111111u, 16 }, { 0x22222222u, 4 } };
    assert(store.initialize(8, users, 2));
    assert(store.is_initialized());
    assert(store.capacity() == 8);
    assert(store.channel_count() == attr::standard_count + 2);
    assert(store.alive_count() == 0);

    // 用户通道按名查找；标准段查不到。
    assert(store.find_channel(0x22222222u) == attr::standard_count + 1);
    assert(store.find_channel(0xDEADBEEFu) == u32_invalid_id);

    const particle_handle h = store.allocate();
    assert(h.is_valid());
    assert(h.row == 0);
    assert(h.generation == 1);
    assert(store.alive_count() == 1);

    // 经 resolve 写入，经 v4_channel 读出（同一段内存）。
    auto* pos = static_cast<math::v4*>(store.resolve(h, attr::position));
    assert(pos != nullptr);
    pos->x = 1.0f;
    pos->y = 2.0f;
    pos->z = 3.0f;
    pos->w = 4.0f;
    const math::v4* pos_direct = store.v4_channel(attr::position);
    assert(pos_direct[0].x == 1.0f && pos_direct[0].w == 4.0f);

    // 用户通道 stride 4 → 对齐 16；独立于标准通道。
    auto* user = static_cast<f32*>(store.resolve(h, attr::standard_count + 1));
    assert(user != nullptr);
    user[0] = 7.5f;
    assert(*static_cast<const f32*>(store.resolve(h, attr::standard_count + 1)) == 7.5f);

    // 跨通道独立性（通道段不得重叠 —— 回归守卫：N0 曾因段偏移按 stride 递增
    // 而非 stride×capacity 导致全通道重叠，此处写死断言）。
    auto* pos0 = store.v4_channel(attr::position);
    auto* vel0 = store.v4_channel(attr::velocity);
    pos0[1] = math::v4{ 9.f, 9.f, 9.f, 9.f };
    vel0[0] = math::v4{ 1.f, 2.f, 3.f, 4.f };
    assert(pos0[1].x == 9.0f && pos0[1].w == 9.0f);
    assert(vel0[0].x == 1.0f && vel0[0].w == 4.0f);

    // 越界通道 id / 无效句柄 → nullptr。
    assert(store.resolve(h, 99) == nullptr);
    assert(store.resolve(particle_handle{}, attr::position) == nullptr);

    store.shutdown();
    assert(!store.is_initialized());
    std::cout << "  PASS: Store channels and resolve" << std::endl;
}

inline void test_store_handle_aba() {
    std::cout << "Testing handle ABA (generation on reuse)..." << std::endl;

    attribute_store store;
    assert(store.initialize(4, nullptr, 0));

    // 满：4 行可分配，第 5 次失败。
    particle_handle hs[4];
    for (u32 i = 0; i < 4; ++i) {
        hs[i] = store.allocate();
        assert(hs[i].is_valid());
        assert(hs[i].generation == 1);
    }
    assert(!store.allocate().is_valid());
    assert(store.is_alive(hs[2]));

    // 死亡即失效（即使行尚未复用）。
    assert(store.free(hs[2]));
    assert(!store.is_alive(hs[2]));
    assert(store.resolve(hs[2], attr::position) == nullptr);

    // 复用同一行：generation 2 ⇒ 旧句柄（gen 1）resolve 失败 —— ABA 断言。
    const particle_handle reused = store.allocate();
    assert(reused.is_valid());
    assert(reused.row == hs[2].row);
    assert(reused.generation == hs[2].generation + 1);
    assert(store.resolve(hs[2], attr::position) == nullptr);
    assert(store.resolve(reused, attr::position) != nullptr);

    // 重复 free 同一句柄：第二次必须失败（不重复入栈）。
    assert(!store.free(hs[2]));
    assert(store.alive_count() == 4);

    // 构造未初始化代数（gen 0）句柄：必须无效。
    const particle_handle gen0{ hs[0].row, 0 };
    assert(!store.is_alive(gen0));
    assert(store.resolve(gen0, attr::position) == nullptr);

    std::cout << "  PASS: Handle ABA protection" << std::endl;
}

// ---- event_bus --------------------------------------------------------------

inline void test_event_bus_fifo_and_overflow() {
    std::cout << "Testing event bus FIFO order and overflow drop..." << std::endl;

    event_bus bus;
    assert(bus.initialize(3));
    assert(bus.pending() == 0);

    const particle_handle h1{ 0, 1 };
    const particle_handle h2{ 1, 1 };
    const particle_handle h3{ 2, 1 };

    bus.push(pfx_event{ h1, 7, event_type::birth, 0.0f });
    bus.push(pfx_event{ h2, 7, event_type::death, 1.0f });
    bus.push(pfx_event{ h3, 8, event_type::birth, 0.5f });
    assert(bus.pending() == 3);

    // 满：新事件丢弃（不覆盖旧事件），计数可见。
    bus.push(pfx_event{ h1, 9, event_type::death, 0.0f });
    assert(bus.pending() == 3);
    assert(bus.dropped_total() == 1);

    // FIFO：顺序与内容逐字段一致。
    pfx_event out[4]{};
    assert(bus.drain(out, 4) == 3);
    assert(out[0].handle == h1 && out[0].type == event_type::birth && out[0].source == 7);
    assert(out[1].handle == h2 && out[1].type == event_type::death && out[1].normalized_age == 1.0f);
    assert(out[2].handle == h3 && out[2].source == 8);
    assert(bus.pending() == 0);

    // 空后再推：环形回绕正确。
    bus.push(pfx_event{ h1, 1, event_type::birth, 0.0f });
    assert(bus.drain(out, 4) == 1 && out[0].source == 1);

    // 空 drain 与零容量缓冲防御。
    assert(bus.drain(out, 4) == 0);
    event_bus empty;
    assert(empty.drain(out, 4) == 0);

    std::cout << "  PASS: Event bus FIFO and overflow" << std::endl;
}

// ---- 发射器生命周期事件序 -----------------------------------------------------

inline void test_emitter_lifecycle_events() {
    std::cout << "Testing emitter birth/death event ordering..." << std::endl;

    assert(initialize(system_config{ 8, 256 }));
    const emitter_id id = emitter_create(emitter_init{ 8, /*seed*/ 42, 0, nullptr });
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);
    assert(em != nullptr);

    // 寿命错开 ⇒ 到期顺序确定：C(0.25) → B(0.5) → A(1.0)。
    const particle_handle a = em->spawn(math::v3{ 1.f, 0.f, 0.f }, math::v3{ 0.f, 1.f, 0.f },
                                        math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 1.0f);
    const particle_handle b = em->spawn(math::v3{ 2.f, 0.f, 0.f }, math::v3{ 0.f, 1.f, 0.f },
                                        math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 0.5f);
    const particle_handle c = em->spawn(math::v3{ 3.f, 0.f, 0.f }, math::v3{ 0.f, 1.f, 0.f },
                                        math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 0.25f);
    assert(a.is_valid() && b.is_valid() && c.is_valid());
    assert(em->alive_count() == 3);

    // birth 事件按 spawn 顺序。
    pfx_event events[16]{};
    u32 n = drain_events(events, 16);
    assert(n == 3);
    for (u32 i = 0; i < n; ++i) {
        assert(events[i].type == event_type::birth);
        assert(events[i].source == id);
    }
    assert(events[0].handle == a && events[1].handle == b && events[2].handle == c);
    assert(pending_events() == 0);

    // spawn 数据落位检查（位置/寿命）。
    const math::v4* pos = em->store().v4_channel(attr::position) + a.row;
    const math::v4* vel = em->store().v4_channel(attr::velocity) + a.row;
    assert(pos->x == 1.0f && pos->w == 0.0f);
    assert(vel->w == 1.0f);

    // 推进 0.3s：仅 C 到期。
    em->tick(0.3f);
    assert(em->alive_count() == 2);
    n = drain_events(events, 16);
    assert(n == 1);
    assert(events[0].type == event_type::death);
    assert(events[0].handle == c);
    assert(events[0].normalized_age == 1.0f);
    // 句柄已死：resolve 拒绝（ADR2 语义）。
    assert(em->store().resolve(events[0].handle, attr::position) == nullptr);

    // 再 0.3s：B 到期（age 0.6 ≥ 0.5）。
    em->tick(0.3f);
    n = drain_events(events, 16);
    assert(n == 1 && events[0].handle == b);
    assert(events[0].normalized_age == 1.0f);

    // 再 0.5s：A 到期（age 1.1 ≥ 1.0）。
    em->tick(0.5f);
    n = drain_events(events, 16);
    assert(n == 1 && events[0].handle == a);
    assert(em->alive_count() == 0);

    // 死亡序：C → B → A（按寿命，与 spawn 序无关）。
    assert(em->elapsed() > 1.0f);

    emitter_destroy(id);
    assert(emitter_get(id) == nullptr);
    assert(emitter_count() == 0);

    shutdown();
    assert(!is_initialized());
    std::cout << "  PASS: Emitter lifecycle events" << std::endl;
}

// ---- 兼容桥：标准通道布局 == 旧 particle_data ---------------------------------

inline void test_legacy_layout_compat() {
    std::cout << "Testing legacy particle_data layout compat bridge..." << std::endl;

    // 80B = 5 × 16B，与旧结构逐字段一致。
    static_assert(sizeof(particles::particle_data) == 80, "旧 particle_data 布局变化，兼容桥需重审");
    static_assert(attr::standard_count * attr::standard_stride == sizeof(particles::particle_data),
                  "标准通道打包尺寸必须等于旧 particle_data");

    attribute_store store;
    assert(store.initialize(4, nullptr, 0));
    const particle_handle h = store.allocate();

    // 写入与旧字段一一对应的值。
    *store.v4_channel(attr::position) = math::v4{ 1.f, 2.f, 3.f, 0.25f };
    (void)h;

    // 按行打包（行 0 即通道基址）：与旧结构逐字段对照。
    particles::particle_data legacy{};
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* vel = store.v4_channel(attr::velocity);
    const math::v4* col = store.v4_channel(attr::color);
    const math::v4* scl = store.v4_channel(attr::scale_rotation);
    const math::v4* uv = store.v4_channel(attr::uv_params);

    legacy.position = pos[0];
    legacy.velocity = vel[0];
    legacy.color = col[0];
    legacy.scale_rotation = scl[0];
    legacy.uv_params = uv[0];

    assert(legacy.position.x == 1.0f && legacy.position.w == 0.25f);
    // memcpy 等价性：打包结果与旧结构逐字节同布局。
    u8 packed[80];
    std::memcpy(packed, pos, 16);
    std::memcpy(packed + 16, vel, 16);
    std::memcpy(packed + 32, col, 16);
    std::memcpy(packed + 48, scl, 16);
    std::memcpy(packed + 64, uv, 16);
    assert(std::memcmp(packed, &legacy, 80) == 0);

    std::cout << "  PASS: Legacy layout compat bridge" << std::endl;
}

// ---- pcg32 确定性 -------------------------------------------------------------

inline void test_pcg32_determinism() {
    std::cout << "Testing pcg32 determinism..." << std::endl;

    pcg32 a, b, c;
    a.seed(42);
    b.seed(42);
    c.seed(43);

    for (u32 i = 0; i < 1000; ++i) {
        const u32 va = a.next_u32();
        const u32 vb = b.next_u32();
        assert(va == vb);          // 同种子 ⇒ 逐值一致
        (void)c.next_u32();
    }

    a.seed(42);
    c.seed(43);
    bool differs = false;
    for (u32 i = 0; i < 64; ++i) {
        if (a.next_u32() != c.next_u32()) { differs = true; break; }
    }
    assert(differs);               // 异种子 ⇒ 序列不同

    a.seed(7);
    for (u32 i = 0; i < 1000; ++i) {
        const f32 f = a.next_f32();
        assert(f >= 0.0f && f < 1.0f);
    }

    std::cout << "  PASS: pcg32 determinism" << std::endl;
}

// ---- 系统级生命周期 -----------------------------------------------------------

inline void test_system_lifecycle_and_drain() {
    std::cout << "Testing system lifecycle, lazy ensure and drain..." << std::endl;

    shutdown();   // 幂等：未初始化时无操作
    assert(!is_initialized());

    // 惰性 ensure：未初始化直接 tick / create 不崩。
    tick(1.0f / 60.0f);
    assert(is_initialized());

    const emitter_id e1 = emitter_create(emitter_init{ 4, 1, 0, nullptr });
    const emitter_id e2 = emitter_create(emitter_init{ 4, 2, 0, nullptr });
    assert(e1 != invalid_emitter && e2 != invalid_emitter && e1 != e2);
    assert(emitter_count() == 2);

    pfx_emitter* em1 = emitter_get(e1);
    assert(em1->spawn(math::v3{ 0.f, 0.f, 0.f }, math::v3{ 0.f, 0.f, 0.f },
                      math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 0.05f)
               .is_valid());
    emitter_get(e2)->spawn(math::v3{ 9.f, 9.f, 9.f }, math::v3{ 0.f, 0.f, 0.f },
                           math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 0.05f);

    // birth × 2（系统级 drain 聚合两发射器，source 区分）。
    pfx_event events[32]{};
    u32 n = drain_events(events, 32);
    assert(n == 2);
    assert(events[0].source == e1 && events[1].source == e2);

    // 系统 tick 驱动到期回收。death 顺序与系统内发射器迭代序无关（unordered_map），
    // 只断言集合语义。
    tick(0.1f);
    n = drain_events(events, 32);
    assert(n == 2);
    assert(events[0].type == event_type::death && events[1].type == event_type::death);
    assert((events[0].source == e1 && events[1].source == e2) ||
           (events[0].source == e2 && events[1].source == e1));
    assert(em1->alive_count() == 0 && emitter_get(e2)->alive_count() == 0);

    // 未知 id 销毁安全；双销毁安全。
    emitter_destroy(0xDEADBEEFu);
    emitter_destroy(e1);
    emitter_destroy(e1);
    assert(emitter_count() == 1);

    // 关闭后 drain 归零、再 shutdown 幂等。
    shutdown();
    assert(drain_events(events, 32) == 0);
    shutdown();

    std::cout << "  PASS: System lifecycle and drain" << std::endl;
}

inline void run_all_tests() {
    test_store_channels_and_resolve();
    test_store_handle_aba();
    test_event_bus_fifo_and_overflow();
    test_emitter_lifecycle_events();
    test_legacy_layout_compat();
    test_pcg32_determinism();
    test_system_lifecycle_and_drain();
    std::cout << "All ParticleFX tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
