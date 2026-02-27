#pragma once

#include "Engine/Particles/ParticlePool.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Particles/ParticleEmitter.h"
#include <cassert>
#include <iostream>

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::test {

inline void test_pool_basic() {
    std::cout << "Testing particle pool basic operations..." << std::endl;
    
    particle_pool pool(1000);
    
    assert(pool.capacity() == 1000);
    assert(pool.allocated_count() == 0);
    assert(pool.is_empty());
    assert(!pool.is_full());
    
    u32 idx = pool.allocate();
    assert(idx != invalid_id);
    assert(pool.allocated_count() == 1);
    assert(!pool.is_empty());
    
    pool.free(idx);
    assert(pool.allocated_count() == 0);
    assert(pool.is_empty());
    
    std::cout << "  PASS: Basic pool operations" << std::endl;
}

inline void test_pool_full() {
    std::cout << "Testing particle pool full capacity..." << std::endl;
    
    const u32 capacity = 100;
    particle_pool pool(capacity);
    
    for (u32 i = 0; i < capacity; ++i) {
        u32 idx = pool.allocate();
        assert(idx != invalid_id);
    }
    
    assert(pool.is_full());
    assert(pool.allocated_count() == capacity);
    
    u32 idx = pool.allocate();
    assert(idx == invalid_id);
    
    pool.free(0);
    assert(!pool.is_full());
    
    idx = pool.allocate();
    assert(idx != invalid_id);
    
    std::cout << "  PASS: Full capacity handling" << std::endl;
}

inline void test_pool_stats() {
    std::cout << "Testing particle pool statistics..." << std::endl;
    
    particle_pool pool(100);
    
    for (u32 i = 0; i < 50; ++i) {
        pool.allocate();
    }
    
    pool_stats stats = pool.stats();
    assert(stats.capacity == 100);
    assert(stats.allocated == 50);
    assert(stats.free_count == 50);
    assert(stats.peak_usage == 50);
    
    for (u32 i = 0; i < 25; ++i) {
        pool.free(i);
    }
    
    stats = pool.stats();
    assert(stats.allocated == 25);
    assert(stats.free_count == 75);
    assert(stats.peak_usage == 50);
    
    std::cout << "  PASS: Pool statistics" << std::endl;
}

inline void test_pool_data_access() {
    std::cout << "Testing particle data access..." << std::endl;
    
    particle_pool pool(10);
    
    u32 idx = pool.allocate();
    particle_data& p = pool.get(idx);
    
    p.position = math::v4{ 1.0f, 2.0f, 3.0f, 0.0f };
    p.velocity = math::v4{ 0.0f, 1.0f, 0.0f, 2.0f };
    p.color = math::v4{ 1.0f, 0.5f, 0.0f, 1.0f };
    p.scale_rotation = math::v4{ 0.5f, 0.5f, 0.0f, 0.0f };
    
    const particle_data& cp = pool.get(idx);
    assert(cp.position.x == 1.0f);
    assert(cp.velocity.w == 2.0f);
    assert(cp.color.g == 0.5f);
    
    std::cout << "  PASS: Particle data access" << std::endl;
}

inline void test_pool_reset() {
    std::cout << "Testing particle pool reset..." << std::endl;
    
    particle_pool pool(50);
    
    for (u32 i = 0; i < 30; ++i) {
        pool.allocate();
    }
    
    assert(pool.allocated_count() == 30);
    
    pool.reset();
    
    assert(pool.allocated_count() == 0);
    assert(pool.is_empty());
    
    std::cout << "  PASS: Pool reset" << std::endl;
}

inline void run_pool_tests() {
    std::cout << "\n=== Particle Pool Tests ===" << std::endl;
    
    test_pool_basic();
    test_pool_full();
    test_pool_stats();
    test_pool_data_access();
    test_pool_reset();
    
    std::cout << "All particle pool tests passed!" << std::endl;
}

inline void test_emitter_basic() {
    std::cout << "Testing emitter basic operations..." << std::endl;
    
    emitter_config config;
    config.max_particles = 100;
    config.spawn_rate = 10.0f;
    
    particle_emitter emitter(config, emitter_id{ 1 });
    
    assert(static_cast<u32>(emitter.get_id()) == 1);
    assert(emitter.is_active());
    assert(emitter.get_active_count() == 0);
    
    emitter.set_active(false);
    assert(!emitter.is_active());
    
    emitter.set_active(true);
    assert(emitter.is_active());
    
    std::cout << "  PASS: Emitter basic operations" << std::endl;
}

inline void test_emitter_config() {
    std::cout << "Testing emitter configuration..." << std::endl;
    
    emitter_config config;
    config.max_particles = 50000;
    config.spawn_rate = 1000.0f;
    config.lifetime_min = 0.5f;
    config.lifetime_max = 2.0f;
    config.velocity_min = math::v3{ -1.0f, 0.0f, -1.0f };
    config.velocity_max = math::v3{ 1.0f, 5.0f, 1.0f };
    config.ring_buffer_mode = true;
    
    particle_emitter emitter(config, emitter_id{ 1 });
    
    const emitter_config& c = emitter.get_config();
    assert(c.max_particles == 50000);
    assert(c.spawn_rate == 1000.0f);
    assert(c.lifetime_min == 0.5f);
    assert(c.lifetime_max == 2.0f);
    
    std::cout << "  PASS: Emitter configuration" << std::endl;
}

inline void test_emitter_transform() {
    std::cout << "Testing emitter transform binding..." << std::endl;
    
    particle_emitter emitter(emitter_config{}, emitter_id{ 1 });
    
    math::v3 pos{ 10.0f, 20.0f, 30.0f };
    emitter.set_position(pos);
    
    const math::v3& p = emitter.get_position();
    assert(p.x == 10.0f);
    assert(p.y == 20.0f);
    assert(p.z == 30.0f);
    
    math::v4 rot{ 0.0f, 0.707f, 0.0f, 0.707f };
    emitter.set_rotation(rot);
    
    const math::v4& r = emitter.get_rotation();
    assert(r.y == 0.707f);
    
    std::cout << "  PASS: Emitter transform binding" << std::endl;
}

inline void run_emitter_tests() {
    std::cout << "\n=== Particle Emitter Tests ===" << std::endl;
    
    test_emitter_basic();
    test_emitter_config();
    test_emitter_transform();
    
    std::cout << "All particle emitter tests passed!" << std::endl;
}

inline void run_all_tests() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "    PARTICLE SYSTEM UNIT TESTS" << std::endl;
    std::cout << "========================================" << std::endl;
    
    run_pool_tests();
    run_emitter_tests();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "    ALL TESTS PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
}

} // namespace primal::particles::test

#else

namespace primal::particles::test {

inline void run_all_tests() {
    std::cout << "Particle system is disabled. Skipping tests." << std::endl;
}

} // namespace primal::particles::test

#endif // !DISABLE_PARTICLE_SYSTEM
