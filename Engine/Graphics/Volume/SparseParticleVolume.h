#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace primal::graphics::volume {
// World-space sphere. Density is extinction-source density, not alpha.
struct VolumeParticle { float x, y, z, radius, density; };
struct SparseVolumeConfig {
    // World units; supported range [0.0001,10000]. Resolve each particle with several voxels.
    float voxel_size = 0.125f;
    uint32_t max_bricks = 4096;
    uint64_t max_voxel_visits = 8'000'000;
};
// GPU ABI: int4 header (count, tiles/axis, 8, 0), float4 voxel size,
// int4 min/max occupied brick, followed by sorted int4(x,y,z,atlas tile).
struct alignas(16) SparseVolumeWord { int32_t x, y, z, w; };
static_assert(sizeof(SparseVolumeWord) == 16);
class SparseParticleVolume {
public:
    // Transactional rebuild: failure leaves the previous snapshot intact.
    bool Build(std::span<const VolumeParticle> particles, const SparseVolumeConfig& config,
               std::string* error = nullptr);
    float Sample(float x, float y, float z) const;
    uint32_t BrickCount() const { return uint32_t(directory[0].x); }
    uint32_t AtlasSide() const { return uint32_t(directory[0].y) * 8; }
    std::vector<SparseVolumeWord> directory = std::vector<SparseVolumeWord>(4);
    std::vector<float> atlas;
private:
    float Voxel(int32_t x, int32_t y, int32_t z) const;
    float voxel_size_ = 0.125f;
};
}
