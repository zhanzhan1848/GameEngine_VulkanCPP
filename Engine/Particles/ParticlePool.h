#pragma once

#include "ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <atomic>
#include <memory>

namespace primal::particles {

class particle_pool {
public:
    explicit particle_pool(u32 capacity);
    ~particle_pool();
    
    DISABLE_COPY_AND_MOVE(particle_pool);
    
    u32 allocate();
    void free(u32 index);
    
    particle_data* data();
    const particle_data* data() const;
    
    particle_data& get(u32 index);
    const particle_data& get(u32 index) const;
    
    pool_stats stats() const;
    u32 capacity() const { return _capacity; }
    u32 allocated_count() const;
    bool is_full() const;
    bool is_empty() const;
    
    void reset();
    
private:
    std::unique_ptr<particle_data[]> _buffer;
    std::unique_ptr<u32[]> _free_list;
    std::atomic<u32> _free_head;
    std::atomic<u32> _allocated_count;
    mutable std::atomic<u32> _peak_usage;
    u32 _capacity;
    
    static constexpr u32 free_list_end{ u32_invalid_id };
};

class particle_pool_buffer {
public:
    particle_pool_buffer() = default;
    ~particle_pool_buffer();
    
    void initialize(u32 capacity_per_frame);
    
    particle_pool& get_frame_pool(u32 frame_index);
    const particle_pool& get_frame_pool(u32 frame_index) const;
    
    pool_stats total_stats() const;
    void reset();
    
    bool is_initialized() const { return _initialized; }
    u32 capacity_per_frame() const { return _capacity_per_frame; }
    
private:
    std::unique_ptr<std::unique_ptr<particle_pool>[]> _frame_pools;
    u32 _capacity_per_frame{ 0 };
    bool _initialized{ false };
};

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
