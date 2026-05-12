#include "CardGenerator.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics::lumen {

void CardGenerator::Initialize(rhi::RHIDeviceBase* device, uint32_t atlas_size, uint32_t page_size, uint32_t max_cards) {
    device_ = device;
    atlas_size_ = atlas_size;
    page_size_ = page_size;
    max_cards_ = max_cards;

    rhi::BufferDesc card_desc{};
    card_desc.size = max_cards * sizeof(SurfaceCacheCard);
    card_desc.type = rhi::BufferType::Structured;
    card_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    card_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    card_desc.structured.elementCount = max_cards;
    card_desc.structured.elementStride = sizeof(SurfaceCacheCard);
    card_data_buffer_ = device_->CreateBuffer(card_desc);

    rhi::BufferDesc lookup_desc{};
    lookup_desc.size = max_cards * sizeof(SurfaceCacheCardLookup);
    lookup_desc.type = rhi::BufferType::Structured;
    lookup_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    lookup_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    lookup_desc.structured.elementCount = max_cards;
    lookup_desc.structured.elementStride = sizeof(SurfaceCacheCardLookup);
    card_lookup_buffer_ = device_->CreateBuffer(lookup_desc);
}

void CardGenerator::RegisterMesh(const math::v3& aabb_min, const math::v3& aabb_max, uint32_t instance_id) {
    MeshInstanceInfo info;
    info.aabb_min = aabb_min;
    info.aabb_max = aabb_max;
    info.instance_id = instance_id;
    info.screen_space_area = 0.0f;
    meshes_.push_back(info);
    allocation_dirty_ = true;
}

void CardGenerator::RebuildCardAllocation() {
    if (!allocation_dirty_) return;

    cards_.clear();
    lookups_.clear();

    uint32_t current_atlas_x = 0;
    uint32_t current_atlas_y = 0;
    uint32_t row_height = 0;

    for (auto& mesh : meshes_) {
        math::v3 half_extent = (mesh.aabb_max - mesh.aabb_min) * 0.5f;
        math::v3 center = (mesh.aabb_min + mesh.aabb_max) * 0.5f;

        SurfaceCacheCardLookup lookup;
        lookup.aabb_min = {mesh.aabb_min.x, mesh.aabb_min.y, mesh.aabb_min.z, 0.0f};
        lookup.aabb_max = {mesh.aabb_max.x, mesh.aabb_max.y, mesh.aabb_max.z, 0.0f};
        lookup.card_start = static_cast<uint32_t>(cards_.size());
        lookup.card_count = 0;
        lookup._pad[0] = lookup._pad[1] = 0;

        // Find max projection area to filter degenerate views
        float max_proj_area = 0.0f;
        for (uint8_t a = 0; a < 3; ++a) {
            float pa;
            if (a == 0) pa = half_extent.y * half_extent.z * 4.0f;
            else if (a == 1) pa = half_extent.x * half_extent.z * 4.0f;
            else pa = half_extent.x * half_extent.y * 4.0f;
            max_proj_area = std::max(max_proj_area, pa);
        }

        for (uint8_t axis = 0; axis < 3; ++axis) {
            for (uint8_t dir = 0; dir < 2; ++dir) {
                float proj_area;
                if (axis == 0) proj_area = half_extent.y * half_extent.z * 4.0f;
                else if (axis == 1) proj_area = half_extent.x * half_extent.z * 4.0f;
                else proj_area = half_extent.x * half_extent.y * 4.0f;

                if (proj_area < 0.01f) continue;
                // Skip views where the object would be too thin (less than 5% of max view)
                if (proj_area < max_proj_area * 0.05f) continue;
                if (cards_.size() >= max_cards_) break;

                SurfaceCacheCard card;
                card.center = {center.x, center.y, center.z, 0.0f};
                card.extent = {half_extent.x, half_extent.y, half_extent.z, 0.0f};
                card.axis_direction = static_cast<uint32_t>(axis) | (static_cast<uint32_t>(dir) << 8);
                card.resolution = 0;
                card._pad0 = 0;
                card.atlas_offset_x = 0;
                card.atlas_offset_y = 0;
                card.mesh_instance_id = mesh.instance_id;
                memset(card._pad1, 0, sizeof(card._pad1));

                float texels_per_unit = 64.0f;
                float proj_size;
                if (axis == 0) proj_size = std::max(half_extent.y, half_extent.z) * 2.0f;
                else if (axis == 1) proj_size = std::max(half_extent.x, half_extent.z) * 2.0f;
                else proj_size = std::max(half_extent.x, half_extent.y) * 2.0f;

                uint32_t res = static_cast<uint32_t>(proj_size * texels_per_unit);
                res = std::max(res, page_size_);
                uint32_t max_card_res = page_size_ * 4;  // cap at 4 pages
                res = std::min(res, max_card_res);
                res = ((res + page_size_ - 1) / page_size_) * page_size_;
                card.resolution = static_cast<uint16_t>(res);

                if (current_atlas_x + res > atlas_size_) {
                    current_atlas_x = 0;
                    current_atlas_y += row_height;
                    row_height = 0;
                }
                // Skip card if it doesn't fit in remaining atlas space
                if (current_atlas_y + res > atlas_size_) continue;

                card.atlas_offset_x = current_atlas_x;
                card.atlas_offset_y = current_atlas_y;
                current_atlas_x += res;
                row_height = std::max(row_height, res);

                cards_.push_back(card);
                lookup.card_count++;
            }
            if (cards_.size() >= max_cards_) break;
        }

        lookups_.push_back(lookup);
    }

    card_count_ = static_cast<uint32_t>(cards_.size());
    UploadToGPU();
    allocation_dirty_ = false;
}

void CardGenerator::UploadToGPU() {
    if (cards_.empty()) return;

    auto* card_data = static_cast<SurfaceCacheCard*>(device_->MapBuffer(card_data_buffer_));
    if (card_data) {
        memcpy(card_data, cards_.data(), cards_.size() * sizeof(SurfaceCacheCard));
        device_->UnmapBuffer(card_data_buffer_);
    }

    if (!lookups_.empty()) {
        auto* lookup_data = static_cast<SurfaceCacheCardLookup*>(device_->MapBuffer(card_lookup_buffer_));
        if (lookup_data) {
            memcpy(lookup_data, lookups_.data(), lookups_.size() * sizeof(SurfaceCacheCardLookup));
            device_->UnmapBuffer(card_lookup_buffer_);
        }
    }
}

} // namespace
