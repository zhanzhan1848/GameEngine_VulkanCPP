#pragma once

#include "CommonHeaders.h"
#include "SurfaceCacheTypes.h"

namespace primal::graphics::lumen {

struct MeshInstanceInfo {
    math::v3  aabb_min;
    math::v3  aabb_max;
    uint32_t  instance_id;
    float     screen_space_area;
};

class CardGenerator {
public:
    CardGenerator() = default;

    void Initialize(rhi::RHIDeviceBase* device, uint32_t atlas_size, uint32_t page_size, uint32_t max_cards);
    void RegisterMesh(const math::v3& aabb_min, const math::v3& aabb_max, uint32_t instance_id);
    void RebuildCardAllocation();

    rhi::ResourceHandle GetCardDataBuffer() const { return card_data_buffer_; }
    rhi::ResourceHandle GetCardLookupBuffer() const { return card_lookup_buffer_; }
    uint32_t GetCardCount() const { return card_count_; }
    uint32_t GetLookupCount() const { return static_cast<uint32_t>(meshes_.size()); }
    const std::vector<SurfaceCacheCard>& GetCards() const { return cards_; }
    const std::vector<SurfaceCacheCardLookup>& GetLookups() const { return lookups_; }

private:
    void UploadToGPU();

    rhi::RHIDeviceBase* device_ = nullptr;
    uint32_t atlas_size_ = 0;
    uint32_t page_size_ = 0;
    uint32_t max_cards_ = 0;

    std::vector<MeshInstanceInfo> meshes_;
    std::vector<SurfaceCacheCard> cards_;
    std::vector<SurfaceCacheCardLookup> lookups_;
    uint32_t card_count_ = 0;
    bool allocation_dirty_ = true;

    rhi::ResourceHandle card_data_buffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::ResourceHandle card_lookup_buffer_ = rhi::handles::INVALID_RESOURCE;
};

} // namespace
