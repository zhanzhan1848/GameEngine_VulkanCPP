#include "Particle.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Particles/ParticleSystem.h"
#include "Particles/ParticleEmitter.h"

namespace primal::particle {

namespace {

struct particle_component {
    particles::emitter_id emitter_id{ particles::invalid_id };
    game_entity::entity_id entity_id{ id::invalid_id };
    bool is_active{ true };
};

std::unordered_map<particle_id, particle_component> component_map;
std::mutex component_mutex;

} // anonymous namespace

component create(init_info info, game_entity::entity entity) {
    particles::emitter_id emitter = particles::create_emitter(info.config);
    
    if (emitter == particles::invalid_id) {
        return component{};
    }
    
    particle_id id{ id::invalid_id };
    {
        std::lock_guard<std::mutex> lock(component_mutex);
        static u32 next_id = 1;
        id = particle_id{ next_id++ };
        
        particle_component comp;
        comp.emitter_id = emitter;
        comp.entity_id = entity.get_id();
        comp.is_active = info.auto_activate;
        
        component_map[id] = comp;
    }
    
    particles::particle_emitter* emitter_ptr = particles::get_emitter(emitter);
    if (emitter_ptr && !info.auto_activate) {
        emitter_ptr->set_active(false);
    }
    
    return component{ id };
}

void remove(component c) {
    if (!c.is_valid()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        particles::destroy_emitter(it->second.emitter_id);
        component_map.erase(it);
    }
}

void update(const component_cache* cache, u32 count) {
    if (!cache || count == 0) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    for (u32 i = 0; i < count; ++i) {
        const component_cache& c = cache[i];
        auto it = component_map.find(c.id);
        
        if (it == component_map.end()) {
            continue;
        }
        
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (!emitter) {
            continue;
        }
        
        if (c.flags & component_flags::spawn_rate) {
            auto config = emitter->get_config();
            config.spawn_rate = c.spawn_rate;
            emitter->set_config(config);
        }
        
        if (c.flags & component_flags::active) {
            emitter->set_active(it->second.is_active);
        }
        
        if (c.flags & component_flags::burst) {
            particles::particle_pool& pool = particles::get_frame_pool(particles::get_frame_index());
            emitter->burst(100, pool);
        }
    }
}

particles::emitter_id get_emitter_id(const component& c) {
    if (!c.is_valid()) {
        return particles::invalid_id;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        return it->second.emitter_id;
    }
    
    return particles::invalid_id;
}

bool is_active(const component& c) {
    if (!c.is_valid()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        return it->second.is_active;
    }
    
    return false;
}

void set_active(component& c, bool active) {
    if (!c.is_valid()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        it->second.is_active = active;
        
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (emitter) {
            emitter->set_active(active);
        }
    }
}

void burst(component& c, u32 count) {
    if (!c.is_valid()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (emitter) {
            particles::particle_pool& pool = particles::get_frame_pool(particles::get_frame_index());
            emitter->burst(count, pool);
        }
    }
}

f32 get_spawn_rate(const component& c) {
    if (!c.is_valid()) {
        return 0.0f;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (emitter) {
            return emitter->get_config().spawn_rate;
        }
    }
    
    return 0.0f;
}

void set_spawn_rate(component& c, f32 rate) {
    if (!c.is_valid()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (emitter) {
            auto config = emitter->get_config();
            config.spawn_rate = rate;
            emitter->set_config(config);
        }
    }
}

u32 get_active_particle_count(const component& c) {
    if (!c.is_valid()) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(component_mutex);
    
    auto it = component_map.find(c.get_id());
    if (it != component_map.end()) {
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter_id);
        if (emitter) {
            return emitter->get_active_count();
        }
    }
    
    return 0;
}

// component class member function implementations
void component::set_active(bool active) {
    particle::set_active(*this, active);
}

bool component::is_active() const {
    return particle::is_active(*this);
}

void component::burst(u32 count) {
    particle::burst(*this, count);
}

void component::set_spawn_rate(f32 rate) {
    particle::set_spawn_rate(*this, rate);
}

f32 component::get_spawn_rate() const {
    return particle::get_spawn_rate(*this);
}

u32 component::get_active_particle_count() const {
    return particle::get_active_particle_count(*this);
}

particles::emitter_id component::get_emitter_id() const {
    return particle::get_emitter_id(*this);
}

} // namespace primal::particle

#endif // !DISABLE_PARTICLE_SYSTEM
