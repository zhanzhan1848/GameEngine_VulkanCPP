#include "ParticleSystem.h"
#include "ParticleEmitter.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <unordered_map>
#include <mutex>
#include <deque>

namespace primal::particles {

namespace {

struct particle_system_state {
    std::unordered_map<u32, std::unique_ptr<particle_emitter>> emitters;
    particle_pool_buffer pool_buffer;
    std::deque<u32> free_ids;
    u32 next_id{ 1 };
    u32 frame_index{ 0 };
    bool initialized{ false };
    std::mutex mutex;
};

particle_system_state* g_state{ nullptr };

}

bool initialize() {
    if (g_state) {
        return true;
    }
    
    g_state = new particle_system_state();
    g_state->pool_buffer.initialize(default_max_particles);
    g_state->initialized = true;
    
    return true;
}

void shutdown() {
    if (!g_state) {
        return;
    }
    
    g_state->emitters.clear();
    g_state->pool_buffer.reset();
    g_state->initialized = false;
    
    delete g_state;
    g_state = nullptr;
}

emitter_id create_emitter(const emitter_config& config) {
    if (!g_state || !g_state->initialized) {
        return emitter_id{ id::invalid_id };
    }
    
    std::lock_guard<std::mutex> lock(g_state->mutex);
    
    u32 id_value;
    if (!g_state->free_ids.empty()) {
        id_value = g_state->free_ids.front();
        g_state->free_ids.pop_front();
    } else {
        id_value = g_state->next_id++;
    }
    
    auto emitter = std::make_unique<particle_emitter>(config, emitter_id{ id_value });
    g_state->emitters[id_value] = std::move(emitter);
    
    return emitter_id{ id_value };
}

void destroy_emitter(emitter_id id) {
    if (!g_state || !g_state->initialized || !id::is_valid(id)) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_state->mutex);
    
    auto it = g_state->emitters.find(id);
    if (it != g_state->emitters.end()) {
        g_state->emitters.erase(it);
        g_state->free_ids.push_back(id);
    }
}

particle_emitter* get_emitter(emitter_id id) {
    if (!g_state || !g_state->initialized || !id::is_valid(id)) {
        return nullptr;
    }
    
    auto it = g_state->emitters.find(id);
    if (it != g_state->emitters.end()) {
        return it->second.get();
    }
    
    return nullptr;
}

const particle_emitter* get_emitter_const(emitter_id id) {
    if (!g_state || !g_state->initialized || !id::is_valid(id)) {
        return nullptr;
    }
    
    auto it = g_state->emitters.find(id);
    if (it != g_state->emitters.end()) {
        return it->second.get();
    }
    
    return nullptr;
}

void update(f32 delta_time) {
    if (!g_state || !g_state->initialized) {
        return;
    }
    
    particle_pool& pool = g_state->pool_buffer.get_frame_pool(g_state->frame_index);
    
    for (auto& [id, emitter] : g_state->emitters) {
        emitter->update(delta_time, pool);
    }
}

void render(u32 frame_index) {
    if (!g_state || !g_state->initialized) {
        return;
    }
    
    g_state->frame_index = frame_index % max_frames_in_flight;
}

u32 get_active_particle_count() {
    if (!g_state || !g_state->initialized) {
        return 0;
    }
    
    return g_state->pool_buffer.get_frame_pool(g_state->frame_index).allocated_count();
}

u32 get_emitter_count() {
    if (!g_state || !g_state->initialized) {
        return 0;
    }
    
    return static_cast<u32>(g_state->emitters.size());
}

pool_stats get_pool_stats() {
    if (!g_state || !g_state->initialized) {
        return pool_stats{};
    }
    
    return g_state->pool_buffer.total_stats();
}

void set_frame_index(u32 frame_index) {
    if (!g_state) {
        return;
    }
    
    g_state->frame_index = frame_index % max_frames_in_flight;
}

u32 get_frame_index() {
    if (!g_state) {
        return 0;
    }
    
    return g_state->frame_index;
}

particle_pool& get_frame_pool(u32 frame_index) {
    static particle_pool dummy_pool(1);
    
    if (!g_state || !g_state->initialized) {
        return dummy_pool;
    }
    
    return g_state->pool_buffer.get_frame_pool(frame_index % max_frames_in_flight);
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
