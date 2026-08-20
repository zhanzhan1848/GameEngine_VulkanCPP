#include "Particle.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Particles/ParticleSystem.h"
#include "Particles/ParticleEmitter.h"

namespace primal::particle {

namespace {

struct particle_component {
    particles::emitter_id emitter{ particles::invalid_id };
    game_entity::entity_id entity_id{ id::invalid_id };
    bool is_active{ true };
};

std::unordered_map<particle_id, particle_component, particle_id_hash> component_map;
std::unordered_map<id::id_type, particle_id> entity_to_particle;
std::mutex component_mutex;

} // anonymous namespace

component create(init_info info, game_entity::entity entity) {
    const particles::emitter_id new_emitter = particles::create_emitter(info.config);
    
    if (new_emitter == particles::invalid_id) {
        return component{};
    }
    
    particle_id id{ id::invalid_id };
    {
        std::lock_guard<std::mutex> lock(component_mutex);
        static u32 next_id = 1;
        id = particle_id{ next_id++ };
        
        particle_component comp;
        comp.emitter = new_emitter;
        comp.entity_id = entity.get_id();
        comp.is_active = info.auto_activate;
        
        component_map[id] = comp;
        entity_to_particle[entity.get_id()] = id;
    }
    
    particles::particle_emitter* emitter_ptr = particles::get_emitter(new_emitter);
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
        entity_to_particle.erase(it->second.entity_id);
        particles::destroy_emitter(it->second.emitter);
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
        
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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
        return it->second.emitter;
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
        
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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
        particles::particle_emitter* emitter = particles::get_emitter(it->second.emitter);
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

component get_component_for_entity(game_entity::entity_id eid) {
    std::lock_guard<std::mutex> lock(component_mutex);
    auto it = entity_to_particle.find(eid);
    if (it != entity_to_particle.end()) {
        return component{ it->second };
    }
    return component{};
}

void remove_for_entity(game_entity::entity_id eid) {
    std::lock_guard<std::mutex> lock(component_mutex);
    auto it = entity_to_particle.find(eid);
    if (it != entity_to_particle.end()) {
        particle_id pid = it->second;
        entity_to_particle.erase(it);

        auto comp_it = component_map.find(pid);
        if (comp_it != component_map.end()) {
            particles::destroy_emitter(comp_it->second.emitter);
            component_map.erase(comp_it);
        }
    }
}

} // namespace primal::particle

#endif // !DISABLE_PARTICLE_SYSTEM
