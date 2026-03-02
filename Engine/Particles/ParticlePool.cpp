#include "ParticlePool.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cassert>
#include <cstring>

namespace primal::particles {

particle_pool::particle_pool(u32 capacity)
    : _capacity(capacity)
    , _free_head(0)
    , _allocated_count(0)
    , _peak_usage(0)
{
    assert(capacity > 0 && "Pool capacity must be greater than 0");
    
    _buffer = std::make_unique<particle_data[]>(capacity);
    _free_list = std::make_unique<u32[]>(capacity);
    
    for (u32 i = 0; i < capacity - 1; ++i) {
        _free_list[i] = i + 1;
    }
    _free_list[capacity - 1] = free_list_end;
    
    _free_head.store(0, std::memory_order_release);
    std::memset(_buffer.get(), 0, sizeof(particle_data) * capacity);
}

particle_pool::~particle_pool() = default;

u32 particle_pool::allocate() {
    u32 index = _free_head.load(std::memory_order_acquire);
    
    while (index != free_list_end) {
        u32 next_free = _free_list[index];
        
        if (_free_head.compare_exchange_weak(index, next_free,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            
            u32 count = _allocated_count.fetch_add(1, std::memory_order_relaxed) + 1;
            
            u32 peak = _peak_usage.load(std::memory_order_relaxed);
            while (count > peak && !_peak_usage.compare_exchange_weak(
                peak, count, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
            
            std::memset(&_buffer[index], 0, sizeof(particle_data));
            return index;
        }
    }
    
    return invalid_id;
}

void particle_pool::free(u32 index) {
    if (index >= _capacity) {
        assert(false && "Invalid particle index");
        return;
    }
    
    u32 old_head = _free_head.load(std::memory_order_acquire);
    
    do {
        _free_list[index] = old_head;
    } while (!_free_head.compare_exchange_weak(old_head, index,
        std::memory_order_acq_rel, std::memory_order_acquire));
    
    _allocated_count.fetch_sub(1, std::memory_order_relaxed);
}

particle_data* particle_pool::data() {
    return _buffer.get();
}

const particle_data* particle_pool::data() const {
    return _buffer.get();
}

particle_data& particle_pool::get(u32 index) {
    assert(index < _capacity && "Particle index out of bounds");
    return _buffer[index];
}

const particle_data& particle_pool::get(u32 index) const {
    assert(index < _capacity && "Particle index out of bounds");
    return _buffer[index];
}

pool_stats particle_pool::stats() const {
    pool_stats s;
    s.capacity = _capacity;
    s.allocated = _allocated_count.load(std::memory_order_relaxed);
    s.peak_usage = _peak_usage.load(std::memory_order_relaxed);
    s.free_count = _capacity - s.allocated;
    return s;
}

u32 particle_pool::allocated_count() const {
    return _allocated_count.load(std::memory_order_relaxed);
}

bool particle_pool::is_full() const {
    return _free_head.load(std::memory_order_acquire) == free_list_end;
}

bool particle_pool::is_empty() const {
    return _allocated_count.load(std::memory_order_relaxed) == 0;
}

void particle_pool::reset() {
    for (u32 i = 0; i < _capacity - 1; ++i) {
        _free_list[i] = i + 1;
    }
    _free_list[_capacity - 1] = free_list_end;
    
    _free_head.store(0, std::memory_order_release);
    _allocated_count.store(0, std::memory_order_release);
}

particle_pool_buffer::~particle_pool_buffer() {
    if (_frame_pools) {
        for (u32 i = 0; i < max_frames_in_flight; ++i) {
            _frame_pools[i].reset();
        }
    }
}

void particle_pool_buffer::initialize(u32 capacity_per_frame) {
    if (_initialized) {
        return;
    }
    
    _capacity_per_frame = capacity_per_frame;
    _frame_pools = std::make_unique<std::unique_ptr<particle_pool>[]>(max_frames_in_flight);
    
    for (u32 i = 0; i < max_frames_in_flight; ++i) {
        _frame_pools[i] = std::make_unique<particle_pool>(capacity_per_frame);
    }
    
    _initialized = true;
}

particle_pool& particle_pool_buffer::get_frame_pool(u32 frame_index) {
    assert(_initialized && "Pool buffer not initialized");
    assert(frame_index < max_frames_in_flight && "Frame index out of bounds");
    return *_frame_pools[frame_index];
}

const particle_pool& particle_pool_buffer::get_frame_pool(u32 frame_index) const {
    assert(_initialized && "Pool buffer not initialized");
    assert(frame_index < max_frames_in_flight && "Frame index out of bounds");
    return *_frame_pools[frame_index];
}

pool_stats particle_pool_buffer::total_stats() const {
    pool_stats total;
    
    if (!_initialized) {
        return total;
    }
    
    for (u32 i = 0; i < max_frames_in_flight; ++i) {
        pool_stats frame_stats = _frame_pools[i]->stats();
        total.capacity += frame_stats.capacity;
        total.allocated += frame_stats.allocated;
        total.peak_usage = (frame_stats.peak_usage > total.peak_usage) 
            ? frame_stats.peak_usage : total.peak_usage;
    }
    
    total.free_count = total.capacity - total.allocated;
    return total;
}

void particle_pool_buffer::reset() {
    if (!_initialized) {
        return;
    }
    
    for (u32 i = 0; i < max_frames_in_flight; ++i) {
        _frame_pools[i]->reset();
    }
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
