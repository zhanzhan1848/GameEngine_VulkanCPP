#include "ParticleCPU.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "ParticleSystem.h"
#include "JobSystem/JobSystem.h"

namespace primal::particles {

namespace {
particle_cpu_processor* g_cpu_processor{ nullptr };
}

void spawn_request_queue::push(const spawn_request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(request);
}

bool spawn_request_queue::pop(spawn_request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    request = queue_.front();
    queue_.pop_front();
    return true;
}

u32 spawn_request_queue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<u32>(queue_.size());
}

void spawn_request_queue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

bool spawn_request_queue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

particle_cpu_processor::particle_cpu_processor() = default;

void particle_cpu_processor::initialize() {
    if (initialized_) {
        return;
    }
    
    spawn_queue_.clear();
    collision_callback_ = nullptr;
    initialized_ = true;
}

void particle_cpu_processor::shutdown() {
    if (!initialized_) {
        return;
    }
    
    spawn_queue_.clear();
    collision_callback_ = nullptr;
    initialized_ = false;
}

void particle_cpu_processor::queue_spawn_requests(const spawn_request* requests, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        spawn_queue_.push(requests[i]);
    }
}

void particle_cpu_processor::process_spawn_queue() {
    if (spawn_queue_.empty()) {
        return;
    }
    
    // Get total count and prepare batch
    const u32 total_count = spawn_queue_.size();
    
    // Copy requests to a vector for parallel processing
    std::vector<spawn_request> requests;
    requests.reserve(total_count);
    
    spawn_request req;
    while (spawn_queue_.pop(req)) {
        requests.push_back(req);
    }
    
    if (requests.empty()) {
        return;
    }
    
    particle_pool& pool = get_frame_pool(get_frame_index());
    
    // Check if JobSystem is available
    if (primal::jobsystem::JobSystem::Get() && primal::jobsystem::JobSystem::IsRunning()) {
        // Use JobSystem for parallel spawn processing
        // We process in batches to minimize contention on the pool allocator
        constexpr u32 batch_size = 64;
        const u32 num_batches = (total_count + batch_size - 1) / batch_size;
        
        auto job_handle = primal::jobsystem::JobSystem::ParallelFor(
            num_batches,
            [&pool, &requests, batch_size, total_count](u32 batch_index) {
                const u32 start = batch_index * batch_size;
                const u32 end = std::min(start + batch_size, total_count);
                
                for (u32 i = start; i < end; ++i) {
                    const spawn_request& request = requests[i];
                    
                    u32 index = pool.allocate();
                    if (index == invalid_id) {
                        break;
                    }
                    
                    particle_data& p = pool.get(index);
                    p.position.x = request.position.x;
                    p.position.y = request.position.y;
                    p.position.z = request.position.z;
                    p.position.w = 0.0f;
                    
                    p.velocity.x = request.velocity.x;
                    p.velocity.y = request.velocity.y;
                    p.velocity.z = request.velocity.z;
                    p.velocity.w = request.lifetime;
                    
                    p.color = request.color;
                    p.scale_rotation.x = request.scale.x;
                    p.scale_rotation.y = request.scale.y;
                    p.scale_rotation.z = request.rotation;
                    p.scale_rotation.w = 0.0f;
                }
            },
            primal::jobsystem::JobPriority::Normal
        );
        
        // Wait for all spawn jobs to complete
        job_handle.Wait();
    } else {
        // Fallback to single-threaded processing
        for (const auto& request : requests) {
            u32 index = pool.allocate();
            if (index == invalid_id) {
                break;
            }
            
            particle_data& p = pool.get(index);
            p.position.x = request.position.x;
            p.position.y = request.position.y;
            p.position.z = request.position.z;
            p.position.w = 0.0f;
            
            p.velocity.x = request.velocity.x;
            p.velocity.y = request.velocity.y;
            p.velocity.z = request.velocity.z;
            p.velocity.w = request.lifetime;
            
            p.color = request.color;
            p.scale_rotation.x = request.scale.x;
            p.scale_rotation.y = request.scale.y;
            p.scale_rotation.z = request.rotation;
            p.scale_rotation.w = 0.0f;
        }
    }
}

void particle_cpu_processor::set_collision_callback(std::function<bool(math::v3&, math::v3&)> callback) {
    collision_callback_ = callback;
}

void particle_cpu_processor::sync_frame_data(u32 frame_index) {
    set_frame_index(frame_index);
}

u32 particle_cpu_processor::get_pending_spawn_count() const {
    return spawn_queue_.size();
}

bool initialize_cpu_processor() {
    if (g_cpu_processor) {
        return true;
    }
    
    g_cpu_processor = new particle_cpu_processor();
    g_cpu_processor->initialize();
    return true;
}

void shutdown_cpu_processor() {
    if (!g_cpu_processor) {
        return;
    }
    
    g_cpu_processor->shutdown();
    delete g_cpu_processor;
    g_cpu_processor = nullptr;
}

particle_cpu_processor* get_cpu_processor() {
    return g_cpu_processor;
}

void queue_spawns(const spawn_request* requests, u32 count) {
    if (g_cpu_processor) {
        g_cpu_processor->queue_spawn_requests(requests, count);
    }
}

void process_queued_spawns() {
    if (g_cpu_processor) {
        g_cpu_processor->process_spawn_queue();
    }
}

u32 get_pending_spawn_count() {
    if (g_cpu_processor) {
        return g_cpu_processor->get_pending_spawn_count();
    }
    return 0;
}

void set_collision_handler(std::function<bool(math::v3&, math::v3&)> handler) {
    if (g_cpu_processor) {
        g_cpu_processor->set_collision_callback(handler);
    }
}

bool has_collision_handler() {
    return g_cpu_processor && g_cpu_processor->has_collision_support();
}

void sync_frame(u32 frame_index) {
    if (g_cpu_processor) {
        g_cpu_processor->sync_frame_data(frame_index);
    }
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
