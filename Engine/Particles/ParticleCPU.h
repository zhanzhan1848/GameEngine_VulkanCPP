#pragma once

#include "ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <functional>
#include <mutex>
#include <deque>

namespace primal::particles {

class spawn_request_queue {
public:
    void push(const spawn_request& request);
    bool pop(spawn_request& request);
    u32 size() const;
    void clear();
    bool empty() const;
    
private:
    std::deque<spawn_request> queue_;
    mutable std::mutex mutex_;
};

class particle_cpu_processor {
public:
    particle_cpu_processor();
    ~particle_cpu_processor() = default;
    
    void initialize();
    void shutdown();
    
    void queue_spawn_requests(const spawn_request* requests, u32 count);
    void process_spawn_queue();
    
    void set_collision_callback(std::function<bool(math::v3&, math::v3&)> callback);
    bool has_collision_support() const { return collision_callback_ != nullptr; }
    
    void sync_frame_data(u32 frame_index);
    
    u32 get_pending_spawn_count() const;
    
private:
    spawn_request_queue spawn_queue_;
    std::function<bool(math::v3&, math::v3&)> collision_callback_;
    bool initialized_{ false };
};

bool initialize_cpu_processor();
void shutdown_cpu_processor();
particle_cpu_processor* get_cpu_processor();

void queue_spawns(const spawn_request* requests, u32 count);
void process_queued_spawns();
u32 get_pending_spawn_count();

void set_collision_handler(std::function<bool(math::v3&, math::v3&)> handler);
bool has_collision_handler();

void sync_frame(u32 frame_index);

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
