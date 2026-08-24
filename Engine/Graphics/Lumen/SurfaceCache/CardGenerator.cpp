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

    // Sort lookups by AABB volume descending — largest meshes first.
    // The trace shader checks only the first N lookups (kMaxLookups),
    // so putting the largest meshes first maximizes coverage.
    // Must also remap card_start indices after sort.
    if (lookups_.size() > 1) {
        // Compute original card_start offsets
        std::vector<u32> original_card_starts;
        original_card_starts.reserve(lookups_.size());
        for (auto& lk : lookups_) original_card_starts.push_back(lk.card_start);

        // Sort by AABB volume descending
        std::vector<size_t> indices(lookups_.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            auto& la = lookups_[a], lb = lookups_[b];
            float va = (la.aabb_max.x - la.aabb_min.x) *
                       (la.aabb_max.y - la.aabb_min.y) *
                       (la.aabb_max.z - la.aabb_min.z);
            float vb = (lb.aabb_max.x - lb.aabb_min.x) *
                       (lb.aabb_max.y - lb.aabb_min.y) *
                       (lb.aabb_max.z - lb.aabb_min.z);
            return va > vb;
        });

        // Reorder cards to match new lookup order
        std::vector<SurfaceCacheCard> sorted_cards;
        sorted_cards.reserve(cards_.size());
        std::vector<SurfaceCacheCardLookup> sorted_lookups;
        sorted_lookups.reserve(lookups_.size());

        for (size_t idx : indices) {
            auto& lk = lookups_[idx];
            u32 new_start = static_cast<u32>(sorted_cards.size());
            for (u32 c = 0; c < lk.card_count; ++c) {
                sorted_cards.push_back(cards_[lk.card_start + c]);
            }
            lk.card_start = new_start;
            sorted_lookups.push_back(lk);
        }
        cards_ = std::move(sorted_cards);
        lookups_ = std::move(sorted_lookups);
    }

    UploadToGPU();
    BuildCardGrid();
    allocation_dirty_ = false;
}

void CardGenerator::BuildCardGrid() {
    // Grid rebuilds with card allocation — release stale buffers first.
    if (grid_offsets_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(grid_offsets_buffer_);
        grid_offsets_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (grid_cards_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(grid_cards_buffer_);
        grid_cards_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (cards_.empty()) return;

    // World AABB over all cards (a card's world AABB equals its mesh AABB).
    float wminx = 3.402823466e+38f, wminy = wminx, wminz = wminx;
    float wmaxx = -3.402823466e+38f, wmaxy = wmaxx, wmaxz = wmaxx;
    for (const auto& c : cards_) {
        wminx = std::min(wminx, c.center.x - c.extent.x);
        wminy = std::min(wminy, c.center.y - c.extent.y);
        wminz = std::min(wminz, c.center.z - c.extent.z);
        wmaxx = std::max(wmaxx, c.center.x + c.extent.x);
        wmaxy = std::max(wmaxy, c.center.y + c.extent.y);
        wmaxz = std::max(wmaxz, c.center.z + c.extent.z);
    }

    // Cell size: aim ≤64 cells per axis, never below 1 world unit.
    const float ex = std::max(wmaxx - wminx, 0.001f);
    const float ey = std::max(wmaxy - wminy, 0.001f);
    const float ez = std::max(wmaxz - wminz, 0.001f);
    const float cell = std::max(1.0f, std::ceil(std::max({ex, ey, ez}) / 64.0f));
    const uint32_t dx = std::max(1u, static_cast<uint32_t>(std::ceil(ex / cell)));
    const uint32_t dy = std::max(1u, static_cast<uint32_t>(std::ceil(ey / cell)));
    const uint32_t dz = std::max(1u, static_cast<uint32_t>(std::ceil(ez / cell)));
    const uint32_t cell_count = dx * dy * dz;

    // Rasterize every card's AABB into the cells it overlaps. Cards_ is
    // sorted by mesh AABB volume descending, so per-cell lists inherit
    // large-mesh-first priority. Cap per cell to bound shader worst case.
    std::vector<std::vector<uint32_t>> per_cell(cell_count);
    for (uint32_t i = 0; i < cards_.size(); ++i) {
        const auto& c = cards_[i];
        const auto clampi = [](float v, int lo, int hi) {
            return std::min(std::max(static_cast<int>(v), lo), hi);
        };
        const int x0 = clampi((c.center.x - c.extent.x - wminx) / cell, 0, int(dx - 1));
        const int x1 = clampi((c.center.x + c.extent.x - wminx) / cell, 0, int(dx - 1));
        const int y0 = clampi((c.center.y - c.extent.y - wminy) / cell, 0, int(dy - 1));
        const int y1 = clampi((c.center.y + c.extent.y - wminy) / cell, 0, int(dy - 1));
        const int z0 = clampi((c.center.z - c.extent.z - wminz) / cell, 0, int(dz - 1));
        const int z1 = clampi((c.center.z + c.extent.z - wminz) / cell, 0, int(dz - 1));
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    auto& list = per_cell[(uint32_t)x + dx * ((uint32_t)y + dy * (uint32_t)z)];
                    if (list.size() < kMaxCardsPerGridCell) list.push_back(i);
                }
            }
        }
    }

    std::vector<uint32_t> offsets(cell_count + 1, 0);
    for (uint32_t i = 0; i < cell_count; ++i) {
        offsets[i + 1] = offsets[i] + static_cast<uint32_t>(per_cell[i].size());
    }
    std::vector<uint32_t> flat_cards;
    flat_cards.reserve(offsets[cell_count]);
    for (uint32_t i = 0; i < cell_count; ++i) {
        for (uint32_t ci : per_cell[i]) flat_cards.push_back(ci);
    }

    // Assemble the offsets buffer: 64B header + prefix sums.
    SurfaceCacheCardGridHeader header{};
    std::memcpy(&header.min_bits[0], &wminx, 4);
    std::memcpy(&header.min_bits[1], &wminy, 4);
    std::memcpy(&header.min_bits[2], &wminz, 4);
    std::memcpy(&header.max_bits[0], &wmaxx, 4);
    std::memcpy(&header.max_bits[1], &wmaxy, 4);
    std::memcpy(&header.max_bits[2], &wmaxz, 4);
    std::memcpy(&header.cell_size_bits, &cell, 4);
    header.card_count = card_count_;
    header.dims[0] = dx; header.dims[1] = dy; header.dims[2] = dz;
    header.header_words = 16;

    std::vector<uint32_t> offsets_buf;
    offsets_buf.reserve(16 + cell_count + 1);
    const auto* hdr_words = reinterpret_cast<const uint32_t*>(&header);
    offsets_buf.insert(offsets_buf.end(), hdr_words, hdr_words + 16);
    offsets_buf.insert(offsets_buf.end(), offsets.begin(), offsets.end());

    auto make_u32_buffer = [&](rhi::ResourceHandle& handle, const std::vector<uint32_t>& data) {
        rhi::BufferDesc desc{};
        desc.size = data.size() * sizeof(uint32_t);
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = static_cast<uint32_t>(data.size());
        desc.structured.elementStride = sizeof(uint32_t);
        handle = device_->CreateBuffer(desc);
        auto* mapped = static_cast<uint32_t*>(device_->MapBuffer(handle));
        if (mapped) {
            std::memcpy(mapped, data.data(), desc.size);
            device_->UnmapBuffer(handle);
        }
    };
    make_u32_buffer(grid_offsets_buffer_, offsets_buf);
    make_u32_buffer(grid_cards_buffer_, flat_cards);
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
