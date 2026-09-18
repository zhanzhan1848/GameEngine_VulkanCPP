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
    info.baked_card_start = 0;
    info.baked_card_count = 0;
    meshes_.push_back(info);
    allocation_dirty_ = true;
}

void CardGenerator::RegisterMeshBaked(const rhi::RHICardBakeData& bake, const math::m4x4& world, uint32_t instance_id) {
    MeshInstanceInfo info;
    info.aabb_min = {3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
    info.aabb_max = {-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
    info.instance_id = instance_id;
    info.screen_space_area = 0.0f;
    info.baked_card_start = static_cast<uint32_t>(baked_cards_pool_.size());
    info.baked_card_count = 0;

    for (const auto& bc : bake.cards) {
        SurfaceCacheCard card;
        memset(&card, 0, sizeof(card));

        // Mesh-local → world (same space the fallback AABBs live in).
        const math::v4 center_w = world * math::v4{bc.center[0], bc.center[1], bc.center[2], 1.0f};
        f32 extent_w[3] = {0.0f, 0.0f, 0.0f};
        for (u32 k = 0; k < 3; ++k) {
            // Axis-aligned box under rotation+scale: project each local
            // half-extent through the absolute column entries (column-major).
            const math::v4& col = world.columns[k];
            const f32 e = bc.extent[k];
            extent_w[0] += std::abs(col.x) * e;
            extent_w[1] += std::abs(col.y) * e;
            extent_w[2] += std::abs(col.z) * e;
        }

        card.center = {center_w.x, center_w.y, center_w.z, 0.0f};
        card.extent = {extent_w[0], extent_w[1], extent_w[2], 0.0f};
        card.axis_direction = bc.axis_direction;
        card.resolution = 0;
        card.mesh_instance_id = instance_id;
        baked_cards_pool_.push_back(card);
        info.baked_card_count++;

        info.aabb_min = {std::min(info.aabb_min.x, center_w.x - extent_w[0]),
                         std::min(info.aabb_min.y, center_w.y - extent_w[1]),
                         std::min(info.aabb_min.z, center_w.z - extent_w[2])};
        info.aabb_max = {std::max(info.aabb_max.x, center_w.x + extent_w[0]),
                         std::max(info.aabb_max.y, center_w.y + extent_w[1]),
                         std::max(info.aabb_max.z, center_w.z + extent_w[2])};
    }

    if (info.baked_card_count == 0) return;   // callers fall back to RegisterMesh
    meshes_.push_back(info);
    allocation_dirty_ = true;
}

void CardGenerator::ResetRegistrations() {
    meshes_.clear();
    baked_cards_pool_.clear();
    page_table_.clear();
    card_last_touch_.clear();
    free_runs_.clear();
    allocation_dirty_ = true;
}

// ============================================================================
// [WRC 计划 Stream E2]: 单实例增量重建。页表关闭 = 更新注册 AABB 后标记
// 全量重分配（管线侧事件驱动路径不再走 snapshot 全量 Rebind——增量收益
// 在管线层）。页表模式 = 额外维护页表/LRU（同一次全量重分配内记账）。
// ============================================================================
bool CardGenerator::RebuildInstance(uint32_t instance_id,
                                    const math::v3& aabb_min, const math::v3& aabb_max) {
    MeshInstanceInfo* target = nullptr;
    for (auto& m : meshes_) {
        if (m.instance_id == instance_id) { target = &m; break; }
    }
    if (!target) return false;
    target->aabb_min = aabb_min;
    target->aabb_max = aabb_max;
    allocation_dirty_ = true;
    return true;
}

void CardGenerator::RebuildCardAllocation() {
    if (!allocation_dirty_) return;

    cards_.clear();
    lookups_.clear();
    baked_mesh_count_ = 0;
    fallback_mesh_count_ = 0;
    // [WRC 计划 Stream D2/D3]: 并行表与自由段随全量重分配重建。
    page_table_.clear();
    card_last_touch_.clear();
    free_runs_.clear();

    uint32_t current_atlas_x = 0;
    uint32_t current_atlas_y = 0;
    uint32_t row_height = 0;

    // T8.13 P3: min-card-resolution env (SC_MIN_CARD_RES, default 32/64 — see
    // the note below). Raised floor for small-surface bounce detail.
    static const u32 sc_min_card_res = [] {
        const char* e = std::getenv("SC_MIN_CARD_RES");
        return e ? (u32)std::max(32, atoi(e)) : 64u;
    }();

    for (auto& mesh : meshes_) {
        const bool baked = mesh.baked_card_count > 0;
        if (baked) ++baked_mesh_count_; else ++fallback_mesh_count_;

        math::v3 half_extent{};
        math::v3 center{};
        if (!baked) {
            half_extent = (mesh.aabb_max - mesh.aabb_min) * 0.5f;
            center = (mesh.aabb_min + mesh.aabb_max) * 0.5f;
        }

        SurfaceCacheCardLookup lookup;
        lookup.aabb_min = {mesh.aabb_min.x, mesh.aabb_min.y, mesh.aabb_min.z, 0.0f};
        lookup.aabb_max = {mesh.aabb_max.x, mesh.aabb_max.y, mesh.aabb_max.z, 0.0f};
        lookup.card_start = static_cast<uint32_t>(cards_.size());
        lookup.card_count = 0;
        lookup._pad[0] = lookup._pad[1] = 0;

        // Projection area of one card view: extent[u]·extent[v]·4 where
        // {u,v} are the two axes perpendicular to the card's axis.
        auto view_area = [](const math::v4& extent, uint32_t axis) -> float {
            const uint32_t u = (axis == 0) ? 1u : 0u;
            const uint32_t v = (axis == 2) ? 1u : 2u;
            const float e[3] = {extent.x, extent.y, extent.z};
            return e[u] * e[v] * 4.0f;
        };

        // Max projection area — degenerate-view filter baseline (same rule
        // for both sources; the bake already applied it offline).
        float max_proj_area = 0.0f;
        if (baked) {
            for (u32 bi = 0; bi < mesh.baked_card_count; ++bi) {
                const SurfaceCacheCard& bc = baked_cards_pool_[mesh.baked_card_start + bi];
                max_proj_area = std::max(max_proj_area,
                                         view_area(bc.extent, bc.axis_direction & 0xFF));
            }
        } else {
            for (uint8_t a = 0; a < 3; ++a) {
                max_proj_area = std::max(max_proj_area, view_area(
                    math::v4{half_extent.x, half_extent.y, half_extent.z, 0.0f}, a));
            }
        }

        bool capacity_full = false;

        // T4.2: shared card emission tail — candidate cards (from the offline
        // bake or the legacy 6-direction derivation below) funnel through the
        // same filters, resolution formula and atlas packing, so the baked
        // path inherits max_cards truncation and shelf-packing behavior.
        auto emit_card = [&](const math::v4& card_center, const math::v4& card_extent,
                             uint32_t axis_direction) -> bool {
            const uint32_t axis = axis_direction & 0xFF;
            const float e[3] = {card_extent.x, card_extent.y, card_extent.z};

            if (view_area(card_extent, axis) < 0.01f) return false;
            if (view_area(card_extent, axis) < max_proj_area * 0.05f) return false;
            if (cards_.size() >= max_cards_) { capacity_full = true; return false; }

            SurfaceCacheCard card;
            card.center = card_center;
            card.extent = card_extent;
            card.axis_direction = axis_direction;
            card.resolution = 0;
            card._pad0 = 0;
            card.atlas_offset_x = 0;
            card.atlas_offset_y = 0;
            card.mesh_instance_id = mesh.instance_id;
            memset(card._pad1, 0, sizeof(card._pad1));

            float texels_per_unit = 64.0f;
            const uint32_t u = (axis == 0) ? 1u : 0u;
            const uint32_t v = (axis == 2) ? 1u : 2u;
            float proj_size = std::max(e[u], e[v]) * 2.0f;

            uint32_t res = static_cast<uint32_t>(proj_size * texels_per_unit);
            uint32_t min_res = std::max(page_size_, sc_min_card_res);
            res = std::max(res, min_res);
            uint32_t max_card_res = page_size_ * 4;  // cap at 4 pages
            res = std::min(res, max_card_res);
            res = ((res + page_size_ - 1) / page_size_) * page_size_;
            card.resolution = static_cast<uint16_t>(res);

            if (current_atlas_x + res > atlas_size_) {
                current_atlas_x = 0;
                current_atlas_y += row_height;
                row_height = 0;
            }
            // T8.13 P3: coverage > detail — a raised card that cannot fit
            // drops back to one page before being skipped entirely.
            if (current_atlas_y + res > atlas_size_ && res > page_size_) {
                res = page_size_;
                card.resolution = static_cast<uint16_t>(res);
            }
            // ==================================================================
            // [WRC 计划 Stream D3]（PRIMAL_SC_PAGE_TABLE=1）: capacity_full
            // 静默丢弃改挤页——货架写满时优先把 LRU 最久未服务的已发大卡
            // （res > page）降档到单页，把腾出的页段推入自由段表，供本卡
            // 重试。降档保留卡片槽位连续性（lookup 区间不变），只损失该卡
            // 的纹理解析度。自由段耗尽才回到丢弃。
            // ==================================================================
            auto emit_from_free_run = [&](uint32_t want) -> bool {
                for (size_t i = 0; i < free_runs_.size(); ++i) {
                    if (free_runs_[i].res >= want) {
                        card.atlas_offset_x = free_runs_[i].x;
                        card.atlas_offset_y = free_runs_[i].y;
                        if (free_runs_[i].res > want) {
                            free_runs_[i].x += want;
                            free_runs_[i].res -= want;
                        } else {
                            free_runs_.erase(free_runs_.begin() + i);
                        }
                        return true;
                    }
                }
                return false;
            };
            bool squeezed = false;
            if (current_atlas_y + res > atlas_size_ && page_table_enabled_) {
                // 找 LRU 最旧的大卡降档（至少释放 (res-page) 宽度的行段）。
                int victim = -1;
                u32 oldest = 0xFFFFFFFFu;
                for (u32 ci = 0; ci < cards_.size(); ++ci) {
                    if (cards_[ci].resolution <= page_size_) continue;
                    const u32 touch = ci < card_last_touch_.size() ? card_last_touch_[ci] : 0u;
                    if (touch < oldest) { oldest = touch; victim = (int)ci; }
                }
                if (victim >= 0) {
                    SurfaceCacheCard& v = cards_[victim];
                    const u32 freed = v.resolution - page_size_;
                    free_runs_.push_back({ v.atlas_offset_x + page_size_,
                                           v.atlas_offset_y, freed });
                    v.resolution = static_cast<uint16_t>(page_size_);
                    if (emit_from_free_run(res)) squeezed = true;
                }
            }
            // 自由段复用（D3 挤页/重发路径）。
            if (!squeezed &&
                (current_atlas_y + res > atlas_size_ || !free_runs_.empty()) &&
                page_table_enabled_ && emit_from_free_run(res)) {
                squeezed = true;
            }
            // Skip card if it still doesn't fit in remaining atlas space
            if (!squeezed && current_atlas_y + res > atlas_size_) return false;

            if (!squeezed) {
                card.atlas_offset_x = current_atlas_x;
                card.atlas_offset_y = current_atlas_y;
                current_atlas_x += res;
                row_height = std::max(row_height, res);
            } else {
                card.resolution = static_cast<uint16_t>(res);
            }

            // [WRC 计划 Stream D2]: 页表记账（页粒度 pack：x_page<<16|y_page）
            // + LRU 新近度表扩容。卡片随后经 sort 重排——并行表随 sort 同步
            // 置换（见下方 sort 块）。
            if (page_table_enabled_) {
                const u32 p = page_size_ ? page_size_ : 1u;
                const u32 px = card.atlas_offset_x / p;
                const u32 py = card.atlas_offset_y / p;
                page_table_.push_back((px << 16) | (py & 0xFFFFu));
                card_last_touch_.push_back(0u);
            }

            cards_.push_back(card);
            lookup.card_count++;
            return true;
        };

        if (baked) {
            // Baked source: emit the world-transformed cards as registered.
            for (u32 bi = 0; bi < mesh.baked_card_count && !capacity_full; ++bi) {
                const SurfaceCacheCard& bc = baked_cards_pool_[mesh.baked_card_start + bi];
                emit_card(bc.center, bc.extent, bc.axis_direction);
            }
        } else {
            // Fallback source: legacy 6-direction AABB derivation.
            for (uint8_t axis = 0; axis < 3 && !capacity_full; ++axis) {
                for (uint8_t dir = 0; dir < 2 && !capacity_full; ++dir) {
                    emit_card(math::v4{center.x, center.y, center.z, 0.0f},
                              math::v4{half_extent.x, half_extent.y, half_extent.z, 0.0f},
                              static_cast<uint32_t>(axis) | (static_cast<uint32_t>(dir) << 8));
                }
                if (cards_.size() >= max_cards_) break;
            }
        }

        lookups_.push_back(lookup);
    }

    card_count_ = static_cast<uint32_t>(cards_.size());

    // T8.13 P3: resolution distribution + packing audit
    {
        u32 bucket[5] = {0, 0, 0, 0, 0};  // 32/64/96/128/other
        u64 texels = 0;
        for (const auto& c : cards_) {
            texels += u64(c.resolution) * c.resolution;
            switch (c.resolution) {
                case 32u: ++bucket[0]; break;
                case 64u: ++bucket[1]; break;
                case 96u: ++bucket[2]; break;
                case 128u: ++bucket[3]; break;
                default: ++bucket[4]; break;
            }
        }
        const u64 capacity = u64(atlas_size_) * atlas_size_;
        printf("[CardGen] dist: 32px=%u 64px=%u 96px=%u 128px=%u | texels=%llu / %llu (%.1f%% of atlas)\n",
               bucket[0], bucket[1], bucket[2], bucket[3],
               (unsigned long long)texels, (unsigned long long)capacity,
               100.0 * double(texels) / double(capacity));
    }

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
        // [WRC 计划 Stream D2]: 页表/LRU 并行表随卡片置换同步重排。
        std::vector<u32> sorted_pages;
        std::vector<u32> sorted_touch;
        if (page_table_enabled_) {
            sorted_pages.reserve(cards_.size());
            sorted_touch.reserve(cards_.size());
        }

        for (size_t idx : indices) {
            auto& lk = lookups_[idx];
            u32 new_start = static_cast<u32>(sorted_cards.size());
            for (u32 c = 0; c < lk.card_count; ++c) {
                const u32 src = lk.card_start + c;
                sorted_cards.push_back(cards_[src]);
                if (page_table_enabled_) {
                    sorted_pages.push_back(src < page_table_.size() ? page_table_[src] : 0u);
                    sorted_touch.push_back(src < card_last_touch_.size() ? card_last_touch_[src] : 0u);
                }
            }
            lk.card_start = new_start;
            sorted_lookups.push_back(lk);
        }
        cards_ = std::move(sorted_cards);
        lookups_ = std::move(sorted_lookups);
        if (page_table_enabled_) {
            page_table_ = std::move(sorted_pages);
            card_last_touch_ = std::move(sorted_touch);
            free_runs_.clear();   // 重排后自由段坐标失效（全量重排即重整）
        }
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

    // [WRC 计划 Stream D2]: 页表缓冲（card slot → 物理页坐标 pack）。
    // v1（无淘汰）页表与 card.atlas_offset 逐字节等价——采样契约切换
    // （SurfaceCacheData.glsl 过表寻址）为 D2 残项，随运行时迁移一起落地。
    if (page_table_enabled_ && !page_table_.empty()) {
        if (page_table_buffer_ == rhi::handles::INVALID_RESOURCE) {
            rhi::BufferDesc desc{};
            desc.size = max_cards_ * sizeof(uint32_t);
            desc.type = rhi::BufferType::Structured;
            desc.usage = rhi::GPUMemoryUsage::Dynamic;
            desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
            desc.structured.elementCount = max_cards_;
            desc.structured.elementStride = sizeof(uint32_t);
            page_table_buffer_ = device_->CreateBuffer(desc);
        }
        if (page_table_buffer_ != rhi::handles::INVALID_RESOURCE) {
            auto* pt = static_cast<uint32_t*>(device_->MapBuffer(page_table_buffer_));
            if (pt) {
                memcpy(pt, page_table_.data(), page_table_.size() * sizeof(uint32_t));
                device_->UnmapBuffer(page_table_buffer_);
            }
        }
    }
}

} // namespace
