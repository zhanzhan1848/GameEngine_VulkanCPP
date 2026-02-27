#include "ParticleSorter.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <algorithm>
#include <cmath>

namespace primal::particles {

f32 particle_sorter::calculate_distance_sq(
    const particle_data& p,
    const math::v3& camera_pos)
{
    const f32 dx = p.position.x - camera_pos.x;
    const f32 dy = p.position.y - camera_pos.y;
    const f32 dz = p.position.z - camera_pos.z;
    return dx * dx + dy * dy + dz * dz;
}

void particle_sorter::sort_by_distance(
    particle_data* particles,
    u32* indices,
    u32 count,
    const math::v3& camera_position)
{
    if (count == 0 || !particles || !indices) return;
    
    // Build distance-index pairs
    std::vector<std::pair<f32, u32>> distances;
    distances.reserve(count);
    
    for (u32 i = 0; i < count; ++i) {
        f32 dist = calculate_distance_sq(particles[i], camera_position);
        distances.emplace_back(dist, i);
    }
    
    // Sort back-to-front (descending distance - far objects first)
    std::sort(distances.begin(), distances.end(), 
        [](const auto& a, const auto& b) {
            return a.first > b.first;  // Greater distance = render first
        });
    
    // Write sorted indices
    for (u32 i = 0; i < count; ++i) {
        indices[i] = distances[i].second;
    }
}

void particle_sorter::sort_parallel(
    particle_data* particles,
    u32* indices,
    u32 count,
    const math::v3& camera_position,
    jobsystem::JobSystem& js)
{
    if (count == 0 || !particles || !indices) return;
    
    // For small counts, use sequential sort
    if (count < 1000) {
        sort_by_distance(particles, indices, count, camera_position);
        return;
    }
    
    // Parallel distance calculation
    std::vector<f32> distances(count);
    std::atomic<u32> completion_count{0};
    
    const u32 chunk_size = 256;
    const u32 num_chunks = (count + chunk_size - 1) / chunk_size;
    
    auto calc_job = js.CreateJob([&](void*) {
        for (u32 chunk = 0; chunk < num_chunks; ++chunk) {
            u32 start = chunk * chunk_size;
            u32 end = std::min(start + chunk_size, count);
            
            js.CreateJob([&, start, end](void*) {
                for (u32 i = start; i < end; ++i) {
                    distances[i] = calculate_distance_sq(particles[i], camera_position);
                }
            })->Run();
        }
    });
    
    calc_job->RunAndWait();
    
    // Build pairs and sort (sequential for now - could use parallel sort)
    std::vector<std::pair<f32, u32>> pairs;
    pairs.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        pairs.emplace_back(distances[i], i);
    }
    
    std::sort(pairs.begin(), pairs.end(), 
        [](const auto& a, const auto& b) { return a.first > b.first; });
    
    for (u32 i = 0; i < count; ++i) {
        indices[i] = pairs[i].second;
    }
}

void particle_sorter::sort_in_place(
    particle_data* particles,
    u32 count,
    const math::v3& camera_position)
{
    if (count == 0 || !particles) return;
    
    // Calculate distances
    std::vector<std::pair<f32, u32>> distances;
    distances.reserve(count);
    
    for (u32 i = 0; i < count; ++i) {
        f32 dist = calculate_distance_sq(particles[i], camera_position);
        distances.emplace_back(dist, i);
    }
    
    // Sort back-to-front
    std::sort(distances.begin(), distances.end(), 
        [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Create temporary copy and reorder
    std::vector<particle_data> temp(count);
    for (u32 i = 0; i < count; ++i) {
        temp[i] = particles[distances[i].second];
    }
    
    // Copy back
    std::copy(temp.begin(), temp.end(), particles);
}

void particle_sorter::radix_sort(
    std::vector<std::pair<u32, u32>>& distance_index_pairs,
    u32 count)
{
    if (count <= 1) return;
    
    // 11-bit radix (2048 buckets) for better distribution
    constexpr u32 RADIX_BITS = 11;
    constexpr u32 RADIX_SIZE = 1 << RADIX_BITS;
    constexpr u32 RADIX_MASK = RADIX_SIZE - 1;
    
    std::vector<std::pair<u32, u32>> temp(count);
    
    for (u32 shift = 0; shift < 32; shift += RADIX_BITS) {
        u32 count_buckets[RADIX_SIZE] = {0};
        
        // Count occurrences
        for (u32 i = 0; i < count; ++i) {
            u32 bucket = (distance_index_pairs[i].first >> shift) & RADIX_MASK;
            count_buckets[bucket]++;
        }
        
        // Compute prefix sums
        u32 total = 0;
        for (u32 i = 0; i < RADIX_SIZE; ++i) {
            u32 c = count_buckets[i];
            count_buckets[i] = total;
            total += c;
        }
        
        // Scatter
        for (u32 i = 0; i < count; ++i) {
            u32 bucket = (distance_index_pairs[i].first >> shift) & RADIX_MASK;
            temp[count_buckets[bucket]++] = distance_index_pairs[i];
        }
        
        std::swap(distance_index_pairs, temp);
    }
}

// sorted_index_buffer implementation
void sorted_index_buffer::resize(u32 max_particles) {
    indices_.resize(max_particles);
    distances_.resize(max_particles);
    sorted_indices_.resize(max_particles);
    
    // Initialize identity indices
    for (u32 i = 0; i < max_particles; ++i) {
        indices_[i] = i;
    }
}

void sorted_index_buffer::update(
    const particle_data* particles,
    u32 count,
    const math::v3& camera_position)
{
    if (count == 0 || !particles) return;
    
    // Calculate distances
    for (u32 i = 0; i < count; ++i) {
        const f32 dx = particles[i].position.x - camera_position.x;
        const f32 dy = particles[i].position.y - camera_position.y;
        const f32 dz = particles[i].position.z - camera_position.z;
        distances_[i] = dx * dx + dy * dy + dz * dz;
        sorted_indices_[i] = i;
    }
    
    // Sort indices by distance (back to front)
    std::sort(sorted_indices_.begin(), sorted_indices_.begin() + count,
        [this](u32 a, u32 b) {
            return distances_[a] > distances_[b];  // Far first
        });
    
    // Copy to output
    std::copy(sorted_indices_.begin(), sorted_indices_.begin() + count, indices_.begin());
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
