#include "OwnerCardRanges.h"
#include "CardGenerator.h"
#include "CoverageCardBudget.h"
#include "Common/CoreLogStream.h"
#include "Graphics/RHI/Core/RHIDevice.h"
// R3（Windows 首编 IWYU）：algorithm/cmath/cstring/limits 此前靠传递链。
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

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

void CardGenerator::RegisterMeshBaked(const rhi::RHICardBakeData& bake, const math::m4x4& world, uint32_t instance_id,
                                      uint32_t static_mesh_slot) {
    MeshInstanceInfo info;
    info.geometry_digest = bake.geometry_digest;
    info.aabb_min = {3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
    info.aabb_max = {-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
    info.instance_id = instance_id;
    info.screen_space_area = 0.0f;
    info.baked_card_start = static_cast<uint32_t>(baked_cards_pool_.size());
    info.baked_card_count = 0;
    bool diagonal = true;
    for (u32 k = 0; k < 3; ++k) {
        const auto col = math::column(world, k);
        const f32 xyz[3] = {col.x, col.y, col.z};
        for (u32 j = 0; j < 3; ++j)
            diagonal &= std::isfinite(xyz[j]) && (j == k ? std::abs(xyz[j]) > 1e-6f : std::abs(xyz[j]) < 1e-6f);
    }
    info.static_mesh_slot = diagonal ? static_mesh_slot : MeshInstanceInfo::kNoStaticSlot;
    info.world = world;

    u32 page_card = 0;
    for (const auto& bc : bake.cards) {
        SurfaceCacheCard card;
        memset(&card, 0, sizeof(card));

        // Mesh-local → world (same space the fallback AABBs live in).
        const math::v4 center_w = world * math::v4{bc.center[0], bc.center[1], bc.center[2], 1.0f};
        f32 extent_w[3] = {0.0f, 0.0f, 0.0f};
        for (u32 k = 0; k < 3; ++k) {
            // Axis-aligned box under rotation+scale: project each local
            // half-extent through the absolute column entries (column-major).
            const math::v4 col = math::column(world, k);
            const f32 e = bc.extent[k];
            extent_w[0] += std::abs(col.x) * e;
            extent_w[1] += std::abs(col.y) * e;
            extent_w[2] += std::abs(col.z) * e;
        }

        card.center = {center_w.x, center_w.y, center_w.z, 0.0f};
        card.extent = {extent_w[0], extent_w[1], extent_w[2], 0.0f};
        card.axis_direction = bc.axis_direction;
        if (diagonal) {
            const u32 a = bc.axis_direction & 0xFFu;
            const auto col = math::column(world, a);
            const f32 scale = a == 0u ? col.x : a == 1u ? col.y : col.z;
            if (scale < 0.f) card.axis_direction ^= 1u << 8u;
        }
        card.resolution = 0;
        card.mesh_instance_id = instance_id;
        baked_cards_pool_.push_back(card);
        // [P2] 离线局部帧 + 页卡序号（静态页合成的坐标系来源）。
        baked_local_.push_back({
            {bc.center[0], bc.center[1], bc.center[2]},
            {bc.extent[0], bc.extent[1], bc.extent[2]},
            page_card++, bc.axis_direction });
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
    baked_local_.clear();
    static_refs_.clear();
    static_card_count_ = 0;
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
    static_refs_.clear();
    static_card_count_ = 0;
    baked_mesh_count_ = 0;
    fallback_mesh_count_ = 0;
    capacity_truncated_meshes_ = 0;
    // [WRC 计划 Stream D2/D3]: 并行表与自由段随全量重分配重建。
    page_table_.clear();
    card_last_touch_.clear();
    free_runs_.clear();
    evicted_rects_.clear();   // [P2] 纪元起始清空（SurfaceCachePass 消费后即弃）

    uint32_t current_atlas_x = 0;
    uint32_t current_atlas_y = 0;
    uint32_t row_height = 0;

    // T8.13 P3: min-card-resolution env (SC_MIN_CARD_RES, default 32/64 — see
    // the note below). Raised floor for small-surface bounce detail.
    static const u32 sc_min_card_res = [] {
        const char* e = std::getenv("SC_MIN_CARD_RES");
        return e ? (u32)std::max(32, atoi(e)) : 64u;
    }();

    // Projection area of one card view: extent[u]·extent[v]·4 where
    // {u,v} are the two axes perpendicular to the card's axis.
    auto view_area = [](const math::v4& extent, uint32_t axis) -> float {
        const uint32_t u = (axis == 0) ? 1u : 0u;
        const uint32_t v = (axis == 2) ? 1u : 2u;
        const float e[3] = {extent.x, extent.y, extent.z};
        return e[u] * e[v] * 4.0f;
    };

    // ======================================================================
    // [SC 卡片 Lumen 对齐 Phase 2] 秩轮转预装箱：baked 卡片在 pool 内已按
    // 离线贪心的选取顺序存放（rank 0 = 该 mesh 覆盖面积最大的主卡）。装箱
    // 按"秩为主、mesh 为辅"轮转执行——全部 mesh 的主卡先占 atlas，atlas
    // 耗尽只损失精化卡（rank≥1），而不是注册序靠后的整个 mesh（mesh 序
    // 装箱会让 2532 张烘焙卡只喂饱前 ~40 个 mesh，大墙面全灭）。
    // 装箱结果预存于 baked_placement，主循环按 mesh 连续推卡——lookup 的
    // 连续区间契约（trace 扫 card_start..card_start+count）不变。
    // ======================================================================
    struct BakedPlacement { u32 res = 0, ox = 0, oy = 0; bool placed = false; };
    std::vector<BakedPlacement> baked_placement(baked_cards_pool_.size());
    // Diagnostic-only packing ledger. Each row is the actual rank-interleaved
    // attempt, including failed placements; no atlas decision depends on it.
    const char* pack_trace_path = std::getenv("SC_PACK_TRACE_PATH");
    FILE* pack_trace = pack_trace_path && *pack_trace_path
        ? std::fopen(pack_trace_path, "w") : nullptr;
    if (pack_trace) {
        std::fprintf(pack_trace,
            "step,rank,mesh,owner,pool,axis,dir,requested,before_x,before_y,before_row,placed,res,offset_x,offset_y,after_x,after_y,after_row\n");
    }

    // 共享装箱逻辑（预装箱与 fallback emit 共用同一来源）：过滤 →
    // 分辨率公式 → 货架/挤页 → 写出 atlas 放置。cards_/页表/lookup 的
    // 推入由调用方负责。
    auto pack_placement = [&](const math::v4& card_center,
                                  const math::v4& card_extent,
                                  uint32_t axis_direction,
                                  bool apply_fraction_filter,
                                  float mesh_max_proj_area,
                                  SurfaceCacheCard& card) -> bool {
            const uint32_t axis = axis_direction & 0xFF;
            const float e[3] = {card_extent.x, card_extent.y, card_extent.z};

            if (view_area(card_extent, axis) < 0.01f) return false;
            if (apply_fraction_filter &&
                view_area(card_extent, axis) < mesh_max_proj_area * 0.05f) return false;
            if (cards_.size() >= max_cards_) return false;

            card.center = card_center;
            card.extent = card_extent;
            card.axis_direction = axis_direction;
            card.resolution = 0;
            card._pad0 = 0;
            card.atlas_offset_x = 0;
            card.atlas_offset_y = 0;
            card.mesh_instance_id = 0;
            memset(card._pad1, 0, sizeof(card._pad1));

            const uint32_t u = (axis == 0) ? 1u : 0u;
            const uint32_t v = (axis == 2) ? 1u : 2u;
            float proj_size = std::max(e[u], e[v]) * 2.0f;

            // [SC per-mesh 计划 P3] 单变量密度候选（PRIMAL_SC_TEXELS_PER_UNIT，
            // 默认 64 = 历史行为；候选默认"关"即不设 env）。离线烘焙端
            //（CardSurfaceBake::BakeParams 无 env——离线分辨率必须与运行时
            // 分配一致，见 CardGenerator.cpp 注释）由 sc-bake-surface 显式
            // 参数化；运行时此 env 只影响 atlas 分配密度。
            static const float tpu = [] {
                const char* e = std::getenv("PRIMAL_SC_TEXELS_PER_UNIT");
                if (!e) return 64.0f;
                const float v = static_cast<float>(std::atof(e));
                return (v >= 16.0f && v <= 256.0f) ? v : 64.0f;
            }();
            uint32_t res = static_cast<uint32_t>(proj_size * tpu);
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
            // [WRC 计划 Stream D3]（PRIMAL_SC_PAGE_TABLE=1）: 挤页/自由段
            // 复用（语义同原 emit_card）。
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
                    // [P2] 页复用拒绝旧引用：释放段记录给 SurfaceCachePass 清零。
                    evicted_rects_.push_back({ v.atlas_offset_x + page_size_,
                                               v.atlas_offset_y, freed, page_size_ });
                    v.resolution = static_cast<uint16_t>(page_size_);
                    if (emit_from_free_run(res)) squeezed = true;
                }
            }
            if (!squeezed &&
                (current_atlas_y + res > atlas_size_ || !free_runs_.empty()) &&
                page_table_enabled_ && emit_from_free_run(res)) {
                squeezed = true;
            }
            if (!squeezed && current_atlas_y + res > atlas_size_) return false;

            if (!squeezed) {
                card.atlas_offset_x = current_atlas_x;
                card.atlas_offset_y = current_atlas_y;
                current_atlas_x += res;
                row_height = std::max(row_height, res);
            } else {
                card.resolution = static_cast<uint16_t>(res);
            }
            return true;
    };

    {
        u32 max_rank = 0;
        for (const auto& m : meshes_)
            max_rank = std::max(max_rank, m.baked_card_count);
        const char* res_sort_env = std::getenv("SC_PACK_RES_DESC");
        const bool pack_res_desc = res_sort_env && std::strcmp(res_sort_env, "1") == 0;
        auto requested_for = [&](const SurfaceCacheCard& bc) -> u32 {
            const u32 axis = bc.axis_direction & 0xFFu;
            const float e[3] = {bc.extent.x, bc.extent.y, bc.extent.z};
            const u32 u = axis == 0 ? 1u : 0u;
            const u32 v = axis == 2 ? 1u : 2u;
            // [P3] 与 pack_placement 同源（预排序口径必须与实际分配一致）。
            static const float tpu2 = [] {
                const char* e = std::getenv("PRIMAL_SC_TEXELS_PER_UNIT");
                if (!e) return 64.0f;
                const float v = static_cast<float>(std::atof(e));
                return (v >= 16.0f && v <= 256.0f) ? v : 64.0f;
            }();
            u32 res = static_cast<u32>(std::max(e[u], e[v]) * 2.0f * tpu2);
            res = std::max(res, std::max(page_size_, sc_min_card_res));
            res = std::min(res, page_size_ * 4u);
            return ((res + page_size_ - 1u) / page_size_) * page_size_;
        };
        u32 pack_step = 0;
        bool coverage_packed=false;
        const char* coverage_env=std::getenv("PRIMAL_SC_COVERAGE_PACK");
        const bool has_static_mesh=std::any_of(meshes_.begin(),meshes_.end(),[](const auto& mesh) {
            return mesh.static_mesh_slot!=MeshInstanceInfo::kNoStaticSlot;
        });
        const bool coverage_requested=coverage_env ? std::strcmp(coverage_env,"1")==0
            : coverage_packing_default_ && has_static_mesh;
        if(device_->GetPlatform()==rhi::RHIPlatform::Vulkan && coverage_requested) {
            const char* densityEnv=std::getenv("PRIMAL_SC_DENSITY_PACK");
            const bool densityRefinement=densityEnv && std::strcmp(densityEnv,"1")==0;
            std::vector<CoverageCardRequest> requests;
            std::vector<size_t> pool_mesh(baked_cards_pool_.size());
            std::vector<u32> pool_rank(baked_cards_pool_.size());
            for(size_t mi=0;mi<meshes_.size();++mi) for(u32 rank=0;rank<meshes_[mi].baked_card_count;++rank) {
                u32 at=meshes_[mi].baked_card_start+rank;
                const auto& bc=baked_cards_pool_[at];
                const float area=view_area(bc.extent,bc.axis_direction&0xffu);
                if(area<=0.f) continue;
                pool_mesh[at]=mi;pool_rank[at]=rank;
                const u32 axis=bc.axis_direction&0xffu;
                const float side[3]={bc.extent.x,bc.extent.y,bc.extent.z};
                const float span=2.f*std::max(side[axis==0?1:0],side[axis==2?1:2]);
                requests.push_back({at,rank,requested_for(bc),area,span});
            }
            CoverageCardPlan plan;
            coverage_packed=PlanCoverageCards(requests,atlas_size_,page_size_,max_cards_,plan,densityRefinement);
            if(coverage_packed) {
                for(const auto& pl:plan.placements) {
                    baked_placement[pl.pool_index]={pl.resolution,pl.atlas_offset_x,pl.atlas_offset_y,true};
                    if(pack_trace) {
                        const auto& bc=baked_cards_pool_[pl.pool_index];
                        size_t mi=pool_mesh[pl.pool_index];
                        std::fprintf(pack_trace,"%u,%u,%zu,%u,%u,%u,%u,%u,%u,%u,%u,1,%u,%u,%u,%u,%u,%u\n",
                            pack_step++,pool_rank[pl.pool_index],mi,meshes_[mi].instance_id,pl.pool_index,
                            bc.axis_direction&0xffu,(bc.axis_direction>>8)&0xffu,requested_for(bc),
                            pl.atlas_offset_x,pl.atlas_offset_y,pl.resolution,pl.resolution,
                            pl.atlas_offset_x,pl.atlas_offset_y,pl.atlas_offset_x+pl.resolution,pl.atlas_offset_y,pl.resolution);
                    }
                }
                current_atlas_x=plan.cursor_x;current_atlas_y=plan.cursor_y;row_height=plan.row_height;
            }
            PRIMAL_LOG_STREAM(Info,"Render") << "[SC_COVERAGE_PACK] complete=" << coverage_packed
                << " candidates=" << requests.size() << " placed=" << plan.placements.size()
                << " atlas=" << atlas_size_ << " min_resolution=" << page_size_
                << " density_refinement=" << densityRefinement;
        }
        for (u32 rank = 0; !coverage_packed && rank < max_rank; ++rank) {
            std::vector<size_t> ranked_meshes;
            for (size_t mi = 0; mi < meshes_.size(); ++mi)
                if (rank < meshes_[mi].baked_card_count) ranked_meshes.push_back(mi);
            if (pack_res_desc) {
                std::stable_sort(ranked_meshes.begin(), ranked_meshes.end(), [&](size_t a, size_t b) {
                    return requested_for(baked_cards_pool_[meshes_[a].baked_card_start + rank]) >
                           requested_for(baked_cards_pool_[meshes_[b].baked_card_start + rank]);
                });
            }
            for (size_t mi : ranked_meshes) {
                const auto& m = meshes_[mi];
                const u32 pool_idx = m.baked_card_start + rank;
                const SurfaceCacheCard& bc = baked_cards_pool_[pool_idx];
                const u32 axis = bc.axis_direction & 0xFFu;
                const u32 requested = pack_trace ? requested_for(bc) : 0u;
                const u32 before_x = current_atlas_x, before_y = current_atlas_y;
                const u32 before_row = row_height;
                SurfaceCacheCard tmp{};
                const bool placed = pack_placement(bc.center, bc.extent, bc.axis_direction,
                                                   /*apply_fraction_filter=*/false, 0.0f, tmp);
                if (placed) {
                    baked_placement[pool_idx] = { tmp.resolution, tmp.atlas_offset_x,
                                                  tmp.atlas_offset_y, true };
                }
                if (pack_trace) {
                    std::fprintf(pack_trace, "%u,%u,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                        pack_step, rank, mi, m.instance_id, pool_idx, axis,
                        (bc.axis_direction >> 8) & 0xFFu, requested,
                        before_x, before_y, before_row, placed ? 1u : 0u,
                        placed ? u32(tmp.resolution) : 0u, tmp.atlas_offset_x,
                        tmp.atlas_offset_y, current_atlas_x, current_atlas_y, row_height);
                }
                ++pack_step;
            }
        }
    }
    if (pack_trace) std::fclose(pack_trace);

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

        // Max projection area — degenerate-view filter baseline (fallback
        // only; the bake already applied its own selection offline).
        float max_proj_area = 0.0f;
        if (!baked) {
            for (uint8_t a = 0; a < 3; ++a) {
                max_proj_area = std::max(max_proj_area, view_area(
                    math::v4{half_extent.x, half_extent.y, half_extent.z, 0.0f}, a));
            }
        }

        bool capacity_full = false;

        // Fallback 发卡（6 向 AABB 推导）：过滤 + 装箱 + 推卡。
        // [SC 卡片 Lumen 对齐 Phase 1]：baked 卡片不走此路径（秩轮转预装箱
        // 见上），且 baked 卡片豁免"最大投影 5%"剔除——v2 的局部小卡正是
        // coverage 贪心的产出，重要性排序已由离线贪心完成。
        auto emit_card = [&](const math::v4& card_center, const math::v4& card_extent,
                             uint32_t axis_direction) -> bool {
            if (cards_.size() >= max_cards_) { capacity_full = true; return false; }
            SurfaceCacheCard card;
            if (!pack_placement(card_center, card_extent, axis_direction,
                                /*apply_fraction_filter=*/true, max_proj_area, card)) {
                return false;
            }
            card.mesh_instance_id = mesh.instance_id;

            // [WRC 计划 Stream D2]: 页表记账 + LRU 新近度表扩容（与推卡
            // 顺序严格同序——sort 重排时并行表同步置换）。
            if (page_table_enabled_) {
                const u32 p = page_size_ ? page_size_ : 1u;
                const u32 px = card.atlas_offset_x / p;
                const u32 py = card.atlas_offset_y / p;
                page_table_.push_back((px << 16) | (py & 0xFFFFu));
                card_last_touch_.push_back(0u);
            }
            card._pad1[0] = MeshInstanceInfo::kNoStaticSlot;
            card._pad1[1] = 0u;
            card._pad1[2] = 0u;
            static_refs_.push_back(CardStaticRef{});
            cards_.push_back(card);
            lookup.card_count++;
            return true;
        };

        if (baked) {
            // Baked source: push the world-transformed cards with their
            // pre-computed rank-interleaved atlas placements (greedy order
            // preserved within the mesh → lookup range stays contiguous).
            for (u32 bi = 0; bi < mesh.baked_card_count; ++bi) {
                if (cards_.size() >= max_cards_) { capacity_full = true; break; }
                const u32 pool_idx = mesh.baked_card_start + bi;
                const BakedPlacement& pl = baked_placement[pool_idx];
                if (!pl.placed) continue;
                SurfaceCacheCard card = baked_cards_pool_[pool_idx];
                card.resolution = static_cast<uint16_t>(pl.res);
                card.atlas_offset_x = pl.ox;
                card.atlas_offset_y = pl.oy;
                card.mesh_instance_id = mesh.instance_id;
                // [P2] 静态页身份：bit0 = static；slot/page_card 供合成与
                // 外来写入隔离（SurfaceCachePass 消费）。静态卡的表面数据
                // 只能来自离线 owner 页（计划 §3.2 生产约束）。
                if (mesh.static_mesh_slot != MeshInstanceInfo::kNoStaticSlot) {
                    card._pad1[0] = mesh.static_mesh_slot;
                    card._pad1[1] = 1u;
                    card._pad1[2] = baked_local_[pool_idx].page_card;
                    CardStaticRef ref;
                    ref.mesh_slot = mesh.static_mesh_slot;
                    ref.page_card = baked_local_[pool_idx].page_card;
                    ref.local_axis_direction = baked_local_[pool_idx].axis_direction;
                    ref.local_center = baked_local_[pool_idx].center;
                    ref.local_extent = baked_local_[pool_idx].extent;
                    ref.world = mesh.world;
                    static_refs_.push_back(ref);
                    ++static_card_count_;
                } else {
                    card._pad1[0] = MeshInstanceInfo::kNoStaticSlot;
                    card._pad1[1] = 0u;
                    card._pad1[2] = 0u;
                    static_refs_.push_back(CardStaticRef{});
                }
                if (page_table_enabled_) {
                    const u32 p = page_size_ ? page_size_ : 1u;
                    const u32 px = card.atlas_offset_x / p;
                    const u32 py = card.atlas_offset_y / p;
                    page_table_.push_back((px << 16) | (py & 0xFFFFu));
                    card_last_touch_.push_back(0u);
                }
                cards_.push_back(card);
                lookup.card_count++;
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
        if (capacity_full) ++capacity_truncated_meshes_;


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

    // ========================================================================
    // [SC 卡片 Lumen 对齐 Phase 0]（2026-09-19 计划）：per-mesh 卡数分布 +
    // 容量截断仪表——一次运行即可回答"每个 mesh 几张卡、谁被 max_cards
    // 截断"。注意 lookup 体积降序排序（下）保住前 kMaxLookups=128 个可被
    // trace 扫描路径命中的 mesh；Sponza 393 lookup > 128 的矛盾以 grid
    // 查询路径为主路径消化（kMaxCardsPerGridCell=8/cell），lookup 仅兜底。
    // ========================================================================
    {
        u32 cards_min = 0xFFFFFFFFu, cards_max = 0, cards_sum = 0;
        u32 hist[5] = {0, 0, 0, 0, 0};   // 1 / 2-4 / 5-8 / 9-12 / >12 cards per mesh
        for (const auto& lk : lookups_) {
            const u32 cnt = lk.card_count;
            cards_min = std::min(cards_min, cnt);
            cards_max = std::max(cards_max, cnt);
            cards_sum += cnt;
            if (cnt <= 1u) ++hist[0];
            else if (cnt <= 4u) ++hist[1];
            else if (cnt <= 8u) ++hist[2];
            else if (cnt <= 12u) ++hist[3];
            else ++hist[4];
        }
        if (!lookups_.empty()) {
            const u32 meshes = (u32)lookups_.size();
            printf("[CardGen] cards/mesh: min=%u avg=%.1f max=%u | 1=%u 2-4=%u 5-8=%u 9-12=%u >12=%u | truncated_by_max_cards=%u\n",
                   cards_min, double(cards_sum) / double(meshes), cards_max,
                   hist[0], hist[1], hist[2], hist[3], hist[4],
                   capacity_truncated_meshes_);
        }
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
        std::vector<CardStaticRef> sorted_static_refs;
        sorted_static_refs.reserve(cards_.size());
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
                if (src < static_refs_.size()) sorted_static_refs.push_back(static_refs_[src]);
                if (page_table_enabled_) {
                    sorted_pages.push_back(src < page_table_.size() ? page_table_[src] : 0u);
                    sorted_touch.push_back(src < card_last_touch_.size() ? card_last_touch_[src] : 0u);
                }
            }
            lk.card_start = new_start;
            sorted_lookups.push_back(lk);
        }
        cards_ = std::move(sorted_cards);
        static_refs_ = std::move(sorted_static_refs);
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
    ++allocation_epoch_;   // [P2] 合成门控：SurfaceCachePass 以 epoch 变化重合成
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


    // Opt-in broadphase experiment: baked projection half-extents may not
    // contain the card's captured depth. Extend only the projection axis
    // through the owner's lookup AABB; the shader still validates each texel.
    const char* axis_env = std::getenv("SC_GRID_OWNER_AXIS");
    const bool preserve_base = axis_env && std::strcmp(axis_env, "2") == 0;
    const bool owner_axis = preserve_base || (axis_env && std::strcmp(axis_env, "1") == 0);
    // Default-off DDGI broadphase diagnostic. The Finalize query accepts
    // captured positions within at least 0.45 m of its SDF hit, so the card
    // cell list can omit an otherwise accepted card at an axial cell edge.
    // Keep the original grid domain and capped list order for a direct A/B.
    const char* band_env = std::getenv("SC_GRID_QUERY_AXIAL_BAND");
    const bool query_band = !owner_axis && band_env && std::strcmp(band_env, "1") == 0;
    struct GridBounds { float minimum[3]; float maximum[3]; };
    std::vector<GridBounds> bounds(cards_.size());
    for (size_t i = 0; i < cards_.size(); ++i) {
        const auto& c = cards_[i];
        bounds[i] = {{c.center.x - c.extent.x, c.center.y - c.extent.y, c.center.z - c.extent.z},
                     {c.center.x + c.extent.x, c.center.y + c.extent.y, c.center.z + c.extent.z}};
    }
    // Mode2 keeps the original grid domain and capped candidate entries.
    // Expanded axial coverage only appends to vacancies; no old card is evicted.
    std::vector<GridBounds> base_bounds;
    if (preserve_base || query_band) base_bounds = bounds;
    if (query_band) {
        for (size_t i = 0; i < cards_.size(); ++i) {
            const uint32_t axis = cards_[i].axis_direction & 0xFFu;
            if (axis >= 3u) continue;
            bounds[i].minimum[axis] -= 0.45f;
            bounds[i].maximum[axis] += 0.45f;
        }
    }
    if (owner_axis) {
        for (const auto& lookup : lookups_) {
            const float lower[3] = {lookup.aabb_min.x, lookup.aabb_min.y, lookup.aabb_min.z};
            const float upper[3] = {lookup.aabb_max.x, lookup.aabb_max.y, lookup.aabb_max.z};
            const size_t begin = std::min<size_t>(lookup.card_start, cards_.size());
            const size_t end = begin + std::min<size_t>(lookup.card_count, cards_.size() - begin);
            for (size_t i = begin; i < end; ++i) {
                const uint32_t axis = cards_[i].axis_direction & 0xFF;
                if (axis >= 3 || !std::isfinite(lower[axis]) || !std::isfinite(upper[axis]) || lower[axis] > upper[axis]) continue;
                bounds[i].minimum[axis] = std::min(bounds[i].minimum[axis], lower[axis]);
                bounds[i].maximum[axis] = std::max(bounds[i].maximum[axis], upper[axis]);
            }
        }
    }

    float wminx = 3.402823466e+38f, wminy = wminx, wminz = wminx;
    float wmaxx = -3.402823466e+38f, wmaxy = wmaxx, wmaxz = wmaxx;
    for (const auto& b : (preserve_base || query_band) ? base_bounds : bounds) {
        wminx = std::min(wminx, b.minimum[0]);
        wminy = std::min(wminy, b.minimum[1]);
        wminz = std::min(wminz, b.minimum[2]);
        wmaxx = std::max(wmaxx, b.maximum[0]);
        wmaxy = std::max(wmaxy, b.maximum[1]);
        wmaxz = std::max(wmaxz, b.maximum[2]);
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
    auto rasterize = [&](const std::vector<GridBounds>& ranges, bool unique) {
    for (uint32_t i = 0; i < cards_.size(); ++i) {
        const auto& b = ranges[i];
        const auto clampi = [](float v, int lo, int hi) {
            return std::min(std::max(static_cast<int>(v), lo), hi);
        };
        const int x0 = clampi((b.minimum[0] - wminx) / cell, 0, int(dx - 1));
        const int x1 = clampi((b.maximum[0] - wminx) / cell, 0, int(dx - 1));
        const int y0 = clampi((b.minimum[1] - wminy) / cell, 0, int(dy - 1));
        const int y1 = clampi((b.maximum[1] - wminy) / cell, 0, int(dy - 1));
        const int z0 = clampi((b.minimum[2] - wminz) / cell, 0, int(dz - 1));
        const int z1 = clampi((b.maximum[2] - wminz) / cell, 0, int(dz - 1));
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    auto& list = per_cell[(uint32_t)x + dx * ((uint32_t)y + dy * (uint32_t)z)];
                    if (list.size() < kMaxCardsPerGridCell &&
                        (!unique || std::find(list.begin(), list.end(), i) == list.end())) list.push_back(i);
                }
            }
        }
    }

    };
    if (preserve_base) {
        rasterize(base_bounds, false);
        rasterize(bounds, true);
    } else {
        rasterize(bounds, false);
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
    owner_directory_ready_=false;
    if (cards_.empty()) return;

    auto* card_data = static_cast<SurfaceCacheCard*>(device_->MapBuffer(card_data_buffer_));
    if (card_data) {
        memcpy(card_data, cards_.data(), cards_.size() * sizeof(SurfaceCacheCard));
        device_->UnmapBuffer(card_data_buffer_);
    }

    if (!lookups_.empty()) {
        std::vector<SurfaceCacheCardLookup> directory;
        const char* requested=std::getenv("PRIMAL_SC_OWNER_CARD_DIRECTORY");
        const bool built=device_->GetPlatform()==rhi::RHIPlatform::Vulkan && requested && std::strcmp(requested,"1")==0 &&
            lookups_.size()==meshes_.size() && BuildOwnerCardDirectory(cards_,lookups_,directory);
        const auto* source=built?directory.data():lookups_.data();
        auto* lookup_data = static_cast<SurfaceCacheCardLookup*>(device_->MapBuffer(card_lookup_buffer_));
        if (lookup_data) {
            memcpy(lookup_data, source, lookups_.size() * sizeof(SurfaceCacheCardLookup));
            device_->UnmapBuffer(card_lookup_buffer_);
            owner_directory_ready_=built && card_data!=nullptr;
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
