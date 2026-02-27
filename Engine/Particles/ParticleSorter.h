#pragma once
#include "Particles/ParticleTypes.h"
#include "Engine/JobSystem/JobSystem.h"
#include <vector>
#include <functional>

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

// -----------------------------------------------------------------------------
// Particle Sorter
// Sorts particles by distance from camera for correct transparency rendering
// -----------------------------------------------------------------------------

class particle_sorter {
public:
    // Sort particles by distance from camera (back to front for transparency)
    static void sort_by_distance(
        particle_data* particles,
        u32* indices,
        u32 count,
        const math::v3& camera_position);
    
    // Parallel sort using JobSystem (for large particle counts)
    static void sort_parallel(
        particle_data* particles,
        u32* indices,
        u32 count,
        const math::v3& camera_position,
        jobsystem::JobSystem& js);
    
    // In-place sort (modifies particle order directly)
    static void sort_in_place(
        particle_data* particles,
        u32 count,
        const math::v3& camera_position);
    
private:
    // Calculate squared distance from camera
    static f32 calculate_distance_sq(
        const particle_data& p,
        const math::v3& camera_pos);
    
    // Radix sort for GPU-friendly sorting (stable, O(n))
    static void radix_sort(
        std::vector<std::pair<u32, u32>>& distance_index_pairs,
        u32 count);
};

// Helper for sorted index buffer management
class sorted_index_buffer {
public:
    sorted_index_buffer() = default;
    
    void resize(u32 max_particles);
    void update(const particle_data* particles, u32 count, const math::v3& camera_position);
    
    const u32* data() const { return indices_.data(); }
    u32* data() { return indices_.data(); }
    u32 size() const { return static_cast<u32>(indices_.size()); }
    
private:
    std::vector<u32> indices_;
    std::vector<f32> distances_;
    std::vector<u32> sorted_indices_;
};

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
