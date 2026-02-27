#pragma once

#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Particles/ParticleEmitter.h"
#include "Engine/Particles/ParticleCPU.h"
#include "Engine/Components/Particle.h"
#include "Engine/EngineAPI/GameEntity.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::examples {

inline void example_basic_emitter() {
    initialize();
    
    emitter_config fire_config;
    fire_config.max_particles = 1000;
    fire_config.mode = emission_mode::continuous;
    fire_config.spawn_rate = 50.0f;
    fire_config.lifetime_min = 0.5f;
    fire_config.lifetime_max = 1.5f;
    fire_config.velocity_min = math::v3{ -0.5f, 2.0f, -0.5f };
    fire_config.velocity_max = math::v3{ 0.5f, 5.0f, 0.5f };
    fire_config.color_start = math::v4{ 1.0f, 0.8f, 0.2f, 1.0f };
    fire_config.color_end = math::v4{ 1.0f, 0.1f, 0.0f, 0.0f };
    fire_config.scale_min = math::v2{ 0.2f, 0.2f };
    fire_config.scale_max = math::v2{ 0.5f, 0.5f };
    fire_config.gravity = math::v3{ 0.0f, -1.0f, 0.0f };
    fire_config.blending = blend_mode::additive;
    fire_config.depth_write = false;
    
    emitter_id fire_emitter = create_emitter(fire_config);
    
    particle_emitter* emitter = get_emitter(fire_emitter);
    if (emitter) {
        emitter->set_position(math::v3{ 0.0f, 0.0f, 0.0f });
        emitter->set_active(true);
    }
    
    for (int frame = 0; frame < 100; ++frame) {
        update(0.016f);
    }
    
    destroy_emitter(fire_emitter);
    shutdown();
}

inline void example_burst_emitter() {
    initialize();
    
    emitter_config explosion_config;
    explosion_config.max_particles = 500;
    explosion_config.mode = emission_mode::burst;
    explosion_config.burst_count = 200;
    explosion_config.lifetime_min = 0.3f;
    explosion_config.lifetime_max = 0.8f;
    explosion_config.velocity_min = math::v3{ -10.0f, -10.0f, -10.0f };
    explosion_config.velocity_max = math::v3{ 10.0f, 10.0f, 10.0f };
    explosion_config.color_start = math::v4{ 1.0f, 1.0f, 1.0f, 1.0f };
    explosion_config.color_end = math::v4{ 1.0f, 0.5f, 0.0f, 0.0f };
    explosion_config.scale_min = math::v2{ 0.1f, 0.1f };
    explosion_config.scale_max = math::v2{ 0.3f, 0.3f };
    explosion_config.gravity = math::v3{ 0.0f, -15.0f, 0.0f };
    explosion_config.blending = blend_mode::additive;
    
    emitter_id explosion = create_emitter(explosion_config);
    
    particle_emitter* emitter = get_emitter(explosion);
    if (emitter) {
        emitter->set_position(math::v3{ 5.0f, 0.0f, 5.0f });
        
        particle_pool& pool = get_frame_pool(get_frame_index());
        emitter->burst(200, pool);
    }
    
    for (int frame = 0; frame < 60; ++frame) {
        update(0.016f);
    }
    
    destroy_emitter(explosion);
    shutdown();
}

inline void example_ring_buffer_emitter() {
    initialize();
    
    emitter_config smoke_config;
    smoke_config.max_particles = 2000;
    smoke_config.mode = emission_mode::ring_buffer;
    smoke_config.ring_buffer_mode = true;
    smoke_config.spawn_rate = 100.0f;
    smoke_config.lifetime_min = 2.0f;
    smoke_config.lifetime_max = 4.0f;
    smoke_config.velocity_min = math::v3{ -0.2f, 0.5f, -0.2f };
    smoke_config.velocity_max = math::v3{ 0.2f, 1.5f, 0.2f };
    smoke_config.color_start = math::v4{ 0.5f, 0.5f, 0.5f, 0.8f };
    smoke_config.color_end = math::v4{ 0.3f, 0.3f, 0.3f, 0.0f };
    smoke_config.scale_min = math::v2{ 0.5f, 0.5f };
    smoke_config.scale_max = math::v2{ 2.0f, 2.0f };
    smoke_config.blending = blend_mode::alpha;
    smoke_config.drag = 0.5f;
    
    emitter_id smoke = create_emitter(smoke_config);
    
    particle_emitter* emitter = get_emitter(smoke);
    if (emitter) {
        emitter->set_position(math::v3{ 0.0f, 0.0f, 0.0f });
    }
    
    for (int frame = 0; frame < 500; ++frame) {
        update(0.016f);
        
        if (frame % 100 == 0) {
            pool_stats stats = get_pool_stats();
        }
    }
    
    destroy_emitter(smoke);
    shutdown();
}

inline void example_ecs_component() {
    particles::initialize();
    
    game_entity::entity_info info{};
    transform::init_info transform_info{};
    transform_info.position[0] = 0.0f;
    transform_info.position[1] = 5.0f;
    transform_info.position[2] = 0.0f;
    info.transform = &transform_info;
    
    particle::init_info particle_info{};
    particle_info.config.max_particles = 500;
    particle_info.config.spawn_rate = 50.0f;
    particle_info.config.blending = blend_mode::additive;
    particle_info.auto_activate = true;
    info.particle = &particle_info;
    
    game_entity::entity entity = game_entity::create(info);
    
    particle::component particle_comp = entity.particle();
    
    if (particle_comp.is_valid()) {
        particle_comp.set_spawn_rate(100.0f);
        
        particle_comp.burst(50);
        
        bool active = particle_comp.is_active();
        particle_comp.set_active(false);
    }
    
    game_entity::remove(entity.get_id());
    particles::shutdown();
}

inline void example_spawn_queue() {
    initialize();
    initialize_cpu_processor();
    
    spawn_request requests[10];
    for (int i = 0; i < 10; ++i) {
        requests[i].position = math::v3{ 
            static_cast<f32>(i), 
            0.0f, 
            static_cast<f32>(i) 
        };
        requests[i].velocity = math::v3{ 0.0f, 1.0f, 0.0f };
        requests[i].color = math::v4{ 1.0f, 1.0f, 1.0f, 1.0f };
        requests[i].scale = math::v2{ 0.1f, 0.1f };
        requests[i].lifetime = 2.0f;
        requests[i].rotation = 0.0f;
    }
    
    queue_spawns(requests, 10);
    
    process_queued_spawns();
    
    shutdown_cpu_processor();
    shutdown();
}

inline void run_all_examples() {
    example_basic_emitter();
    example_burst_emitter();
    example_ring_buffer_emitter();
    example_ecs_component();
    example_spawn_queue();
}

} // namespace primal::particles::examples

#endif // !DISABLE_PARTICLE_SYSTEM
