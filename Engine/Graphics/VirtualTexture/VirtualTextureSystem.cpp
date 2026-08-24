/**
 * @file VirtualTextureSystem.cpp
 * @brief VT M2 — SVT 运行时核心实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-22
 */

#include "VirtualTextureSystem.h"
#include "../RHI/Core/RHIDevice.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <zlib.h>

namespace primal::graphics::vt {

using namespace primal::graphics::rhi;

// ============================================================================
// Metal 版页表更新 kernel(嵌入源码;Vulkan 走 VTSys_UpdatePageTable.comp.spv)
// ============================================================================
static const char* kPTUpdateMSL = R"(
#include <metal_stdlib>
using namespace metal;
kernel void vtSysUpdatePageTable(texture2d<uint, access::read_write> pageTable [[texture(0)]],
                                 device const uint* entries [[buffer(1)]],
                                 uint gid [[thread_position_in_grid]])
{
    uint count = entries[0];
    if (gid >= count) return;
    // 2.7:6 元组条目 — RGBA 四通道一次写(每通道一层物理地址)
    uint ptX = entries[1 + gid * 6 + 0];
    uint ptY = entries[1 + gid * 6 + 1];
    uint4 val(entries[1 + gid * 6 + 2], entries[1 + gid * 6 + 3],
              entries[1 + gid * 6 + 4], entries[1 + gid * 6 + 5]);
    pageTable.write(val, ushort2(ptX, ptY), 0);
}
)";

// GLSL 版(Vulkan 路径,编译产物 Assets/Shaders/VTSys_UpdatePageTable.comp.spv)
static const char* kPTUpdateGLSLPath = "Assets/Shaders/VTSys_UpdatePageTable.comp.spv";

virtual_texture_system& virtual_texture_system::instance() {
    static virtual_texture_system s;
    return s;
}

// ============================================================================
// initialize / shutdown
// ============================================================================
bool virtual_texture_system::initialize(RHIDeviceBase* device, const vt_config& cfg) {
    if (device_ == device) return true;
    shutdown();
    if (!device || !device->IsValid()) return false;
    device_ = device;
    cfg_ = cfg;

    TextureDesc ptDesc{};
    ptDesc.size = {cfg_.page_table_size, cfg_.page_table_size, 1};
    ptDesc.mipLevels = 1;
    ptDesc.arraySize = 1;
    ptDesc.format = DataFormat::RGBA32_UInt;   // 2.7:每通道一层(一次查找取全层)
    ptDesc.type = TextureType::Texture2D;
    ptDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                    TextureUsage::CopySource;   // 调试探针回读
    ptDesc.memoryUsage = GPUMemoryUsage::Static;
    ptDesc.name = "VTSys_PageTable";
    page_table_ = device_->CreateTexture(ptDesc);
    if (page_table_ == handles::INVALID_RESOURCE) { shutdown(); return false; }

    free_subrects_.clear();
    free_subrects_[cfg_.page_table_size].push_back({0, 0});

    const u32 slotCount = cfg_.feedback_latency + 1;
    feedback_.resize(slotCount);
    for (u32 i = 0; i < slotCount; ++i) {
        BufferDesc d{};
        d.size = u64(cfg_.feedback_slots) * sizeof(u32);
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "VTSys_Feedback";
        feedback_[i].buffer = device_->CreateBuffer(d);
        if (feedback_[i].buffer == handles::INVALID_RESOURCE) { shutdown(); return false; }
    }
    if (!readback_ring_.initialize(device_, u64(cfg_.feedback_slots) * sizeof(u32), slotCount)) {
        shutdown();
        return false;
    }

    next_vt_id_ = 1;   // feedback 编码 vtId 为 4 位(≤15) — 新会话从 1 重新编号
    shutdown_requested_ = false;
    worker_ = std::thread([this]() {
        std::unique_lock<std::mutex> lock(stream_mutex_);
        while (!shutdown_requested_) {
            while (stream_queue_.empty() && !shutdown_requested_)
                stream_cv_.wait(lock);
            if (shutdown_requested_) break;
            // producer VT 检查在 pop 之前 — 弹出即丢失(阶段 1.5 只扫队列)
            {
                auto peek = vts_.find(stream_queue_.front().first);
                if (peek != vts_.end() && peek->second.producer != nullptr) {
                    // 留给 update 阶段 1.5 GPU 生产;不删 in_flight。
                    // 解锁让出,避免自旋占满
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    lock.lock();
                    continue;
                }
            }
            const auto [vt_id, page] = stream_queue_.front();
            stream_queue_.pop_front();
            // 临界区内取数据(vts_ 节点引用稳定,但 blob/asset 访问尽量短)
            // 2.7:每层独立 ready_upload(层格式/段偏移各异)
            u32 producedLayers = 0;
            {
                auto it = vts_.find(vt_id);
                if (it == vts_.end() || page >= u32(it->second.asset.page_count())) {
                    stream_in_flight_.erase((u64(vt_id) << 32) | page);
                    continue;
                }
                const vt_entry& vt = it->second;
                const bool useAstc = device_->GetDeviceInfo().supportsASTC;
                for (u32 l = 0; l < vt.layer_count; ++l) {
                    ready_upload up{};
                    up.vt_id = vt_id;
                    up.page = page;
                    up.layer = l;
                    up.format = vt.layer_formats[l];
                    const content::vt::vtex_page_index* pi = vt.asset.page_layer(page, l);
                    u64 sz = 0;
                    const u8* data = pi ? vt.asset.tile_data(*pi, useAstc, sz) : nullptr;
                    if (!data || sz == 0 || up.format == 0) continue;
                    up.data.resize(sz);
                    std::memcpy(up.data.data(), data, sz);
                    up.data_size = sz;
                    if (vt.asset.uses_zlib()) {
                        const u64 blockBytes =
                            (up.format == content::vt::bcn_format::BC1_UNorm ||
                             up.format == content::vt::bcn_format::BC4_UNorm) ? 8 : 16;
                        const u64 raw = 34ull * 34 * blockBytes;
                        std::vector<u8> plain(uLongf(raw), 0u);
                        uLongf plainLen = uLongf(raw);
                        if (uncompress(plain.data(), &plainLen, up.data.data(), uLong(sz)) != Z_OK) {
                            continue;
                        }
                        up.data.swap(plain);
                        up.data_size = plainLen;
                    }
                    upload_ready_.push_back(std::move(up));
                    ++producedLayers;
                }
            }
            if (producedLayers == 0) {
                stream_in_flight_.erase((u64(vt_id) << 32) | page);
            }
        }
    });

    // 页表初始布局:UNDEFINED → ShaderResource(首次采样前必须;renderer 路径
    // 无人补 barrier — 系统自理)
    {
        CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
        if (cb) {
            cb->Reset();
            cb->Begin();
            ResourceBarrier b{};
            b.resource = page_table_;
            b.beforeState = ResourceState::Unknown;
            b.afterState = ResourceState::ShaderResource;
            b.subresource = 0xFFFFFFFF;
            b.queueFamily = 0xFFFFFFFF;
            cb->InsertBarrier(&b, 1);
            cb->End();
            cb->Submit(0);
            cb->WaitForCompletion();
            device_->DestroyCommandBuffer(cmd);
        }
    }

    stats_ = vt_stats{};
    return true;
}

void virtual_texture_system::shutdown() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            shutdown_requested_ = true;
        }
        stream_cv_.notify_all();
        worker_.join();
    }
    if (device_) {
        readback_ring_.shutdown(device_);
        for (auto& f : feedback_)
            if (f.buffer) device_->DestroyBuffer(f.buffer);
        feedback_.clear();
        for (auto& pool : pools_)
            if (pool.atlas) device_->DestroyTexture(pool.atlas);
        pools_.clear();
        format_to_pool_.clear();
        if (pt_update_pipeline_) device_->DestroyPipeline(pt_update_pipeline_);
        if (pt_update_ds_) device_->DestroyDescriptorSet(pt_update_ds_);
        if (pt_update_ds_layout_) device_->DestroyDescriptorSetLayout(pt_update_ds_layout_);
        if (pt_update_layout_) device_->DestroyPipelineLayout(pt_update_layout_);
        if (pt_update_shader_) device_->DestroyShader(pt_update_shader_);
        if (pt_entry_buffer_) device_->DestroyBuffer(pt_entry_buffer_);
        if (page_table_) device_->DestroyTexture(page_table_);
    }
    pt_update_pipeline_ = 0; pt_update_ds_ = 0; pt_update_ds_layout_ = 0;
    pt_update_layout_ = 0; pt_update_shader_ = 0; pt_entry_buffer_ = 0;
    page_table_ = 0;
    vts_.clear();
    free_subrects_.clear();
    stream_queue_.clear();
    stream_in_flight_.clear();
    upload_ready_.clear();
    upload_pending_.clear();
    pt_entries_.clear();
    device_ = nullptr;
    frame_counter_ = 0;
    feedback_frame_counter_ = 0;
}

// ============================================================================
// 页表子矩形分配(2 的幂方块;分裂不合并 — 页表 256 容量下碎片可容忍)
// ============================================================================
bool virtual_texture_system::allocate_page_table_subrect(u32 pages_top, u32& ox, u32& oy) {
    u32 want = pages_top;
    while (want <= cfg_.page_table_size) {
        auto it = free_subrects_.find(want);
        if (it != free_subrects_.end() && !it->second.empty()) {
            std::tie(ox, oy) = it->second.back();
            it->second.pop_back();
            return true;
        }
        u32 bigger = want << 1;
        while (bigger <= cfg_.page_table_size) {
            auto bit = free_subrects_.find(bigger);
            if (bit != free_subrects_.end() && !bit->second.empty()) {
                auto [bx, by] = bit->second.back();
                bit->second.pop_back();
                free_subrects_[want].push_back({bx + want, by});
                free_subrects_[want].push_back({bx, by + want});
                free_subrects_[want].push_back({bx + want, by + want});
                ox = bx; oy = by;
                return true;
            }
            bigger <<= 1;
        }
        want <<= 1;
    }
    return false;
}

void virtual_texture_system::free_page_table_subrect(u32 pages_top, u32 ox, u32 oy) {
    free_subrects_[pages_top].push_back({ox, oy});
}

// ============================================================================
// 注册 / 释放
// ============================================================================
u32 virtual_texture_system::register_texture(const u8* vtex_data, u64 vtex_size) {
    if (!device_) return ~0u;
    vt_entry e;
    e.blob.assign(vtex_data, vtex_data + vtex_size);
    if (!e.asset.initialize(e.blob.data(), e.blob.size())) return ~0u;

    const auto& h = e.asset.header();
    if (h.tile_size != cfg_.tile_size || h.border != cfg_.border) {
        std::cerr << "[VTSys] tile/border 与系统配置不一致(t=" << h.tile_size
                  << " b=" << h.border << ")" << std::endl;
        return ~0u;
    }
    e.pages_top = std::max<u32>(1, h.width / cfg_.tile_size);
    e.layer_count = std::max(1u, h.reserved[0] ? h.reserved[0] : 1u);
    const bool useAstc = device_->GetDeviceInfo().supportsASTC;
    for (u32 l = 0; l < e.layer_count; ++l) {
        // 多层容器:BCn 段格式在 reserved[1+l];ASTC 平台全层统一 ASTC 池
        e.layer_formats[l] = useAstc ? h.astc_format
                                     : (l == 0 ? h.bcn_format
                                               : h.reserved[1 + l]);
    }
    e.page_layer_bits.assign(total_page_count(e.pages_top), 0);
    e.page_layer_origins.assign(total_page_count(e.pages_top) * 4, 0);
    if (!allocate_page_table_subrect(e.pages_top, e.pt_origin_x, e.pt_origin_y)) {
        std::cerr << "[VTSys] 页表空间不足(pages_top=" << e.pages_top << ")" << std::endl;
        return ~0u;
    }
    // feedback 编码 vtId 为 4 位:并发上限 15。直接扫描空闲 id
    // (回收列表与计数器混用会撞车 — emplace 静默失败导致重复映射)
    u32 newId = 0;
    for (u32 candidate = 1; candidate <= 15; ++candidate) {
        if (vts_.find(candidate) == vts_.end()) { newId = candidate; break; }
    }
    if (newId == 0) {
        std::cerr << "[VTSys] 并发 VT 数达 feedback 编码上限 15,拒绝注册" << std::endl;
        free_page_table_subrect(e.pages_top, e.pt_origin_x, e.pt_origin_y);
        return ~0u;
    }
    e.id = newId;
    e.resident_page_to_slot.assign(total_page_count(e.pages_top), ~0u);

    u32 id = e.id;
    {
        std::lock_guard<std::mutex> g(stream_mutex_);
        vts_.emplace(id, std::move(e));
    }
    // 懒建页池:每层一个(ASTC 平台同格式 → 同池;BCn 平台按层格式分池)。
    // M5:空数据段(RVT 合成容器,段格式 0)不建池 — 由
    // register_runtime_texture 按生产格式建
    if (vts_[id].asset.header().bcn_size > 0 || vts_[id].asset.header().astc_size > 0) {
        for (u32 l = 0; l < vts_[id].layer_count; ++l) {
            if (ensure_pool(vts_[id].layer_formats[l]) == ~0u) {
                release_texture(id);
                return ~0u;
            }
        }
    }
    // root 页(coarsest mip,单页)立即入队 — 采样安全网
    const u32 rootPage = total_page_count(vts_[id].pages_top) - 1;
    submit_streaming(id, rootPage);
    stats_.registered = u32(vts_.size());
    return id;
}

u32 virtual_texture_system::register_runtime_texture(u32 width, u32 height,
                                                        u32 format,
                                                        IVTProducer* producer) {
    if (!device_ || !producer) return ~0u;
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) return ~0u;

    // 合成最小 vtex 容器(页网格/mip 语义与磁盘 VT 一致;数据段为空 —
    // 页内容由 producer 生产)
    std::vector<u8> blob(sizeof(content::vt::vtex_header), 0);
    {
        content::vt::vtex_header h{};
        std::memcpy(h.magic, "VTEX", 4);
        h.version = content::vt::kVTEXVersion;
        h.width = width;
        h.height = height;
        h.tile_size = cfg_.tile_size;
        h.border = cfg_.border;
        u32 side = width;
        u32 mips = 0;
        while (side >= cfg_.tile_size && mips < 8) { ++mips; side >>= 1; }
        if (side > 0 && side < cfg_.tile_size && mips > 0) { /* 最低 mip 边长<tile:最后一层单页 */ }
        h.mip_count = std::max(1u, mips);
        h.bcn_format = 0;
        h.astc_format = 0;
        // 页数按金字塔打包(同 total_page_count)
        u32 total = 0, sp = std::max(1u, width / cfg_.tile_size);
        while (sp) { total += sp * sp; sp >>= 1; }
        h.page_count = total;
        h.avg_color = 0xFF808080u;
        h.index_offset = sizeof(content::vt::vtex_header);
        h.bcn_offset = h.index_offset;
        h.bcn_size = 0;
        h.astc_offset = h.index_offset;
        h.astc_size = 0;
        std::memcpy(blob.data(), &h, sizeof(h));
        // 合成容器必须含索引段(读取器结构校验);条目为零 — producer VT
        // 不走 tile_data,索引仅满足布局校验
        blob.resize(sizeof(h) + size_t(total) * sizeof(content::vt::vtex_page_index), 0);
    }

    // 复用 register_texture 的页表分配/池登记,再挂 producer 与格式
    u32 id = register_texture(blob.data(), blob.size());
    if (id == ~0u) return ~0u;
    {
        std::lock_guard<std::mutex> g(stream_mutex_);
        auto it = vts_.find(id);
        if (it == vts_.end()) return ~0u;
        it->second.producer = producer;
        for (u32 l = 0; l < it->second.layer_count; ++l)
            it->second.layer_formats[l] = format;
    }
    // 确保对应池存在(RVT 通常 RGBA8 独立池)
    if (ensure_pool(format) == ~0u) {
        release_texture(id);
        return ~0u;
    }
    // root 页请求(同磁盘 VT 安全网)
    {
        std::lock_guard<std::mutex> g(stream_mutex_);
        const u32 rootPage = total_page_count(vts_[id].pages_top) - 1;
        submit_streaming(id, rootPage);
    }
    stats_.registered = u32(vts_.size());
    return id;
}

u32 virtual_texture_system::ensure_pool(u32 format_value) {
    auto it = format_to_pool_.find(format_value);
    if (it != format_to_pool_.end()) return it->second;

    physical_pool pool;
    pool.format = format_value;
    TextureDesc aDesc{};
    aDesc.size = {cfg_.atlas_size, cfg_.atlas_size, 1};
    aDesc.mipLevels = 1;
    aDesc.arraySize = 1;
    aDesc.format = DataFormat(format_value);
    aDesc.type = TextureType::Texture2D;
    // M5:GPUFinalizer 存储镜像直写需要 UAV — 仅未压缩格式支持
    // (ASTC/BCn 无存储镜像能力,RVT 池用 RGBA8)
    aDesc.usage = TextureUsage::ShaderResource;
    if (DataFormat(format_value) == DataFormat::RGBA8_UNorm ||
        DataFormat(format_value) == DataFormat::BGRA8_UNorm ||
        DataFormat(format_value) == DataFormat::RGBA16_Float ||
        DataFormat(format_value) == DataFormat::R32_UInt) {
        aDesc.usage = aDesc.usage | TextureUsage::UnorderedAccess;
    }
    aDesc.memoryUsage = GPUMemoryUsage::Static;
    aDesc.name = "VTSys_Atlas";
    pool.atlas = device_->CreateTexture(aDesc);
    if (pool.atlas == handles::INVALID_RESOURCE) return ~0u;
    // atlas 初始布局 → ShaderResource(同页表;上传路径自身会再转 CopyDest)
    {
        CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
        if (cb) {
            cb->Reset();
            cb->Begin();
            ResourceBarrier b{};
            b.resource = pool.atlas;
            b.beforeState = ResourceState::Unknown;
            b.afterState = ResourceState::ShaderResource;
            b.subresource = 0xFFFFFFFF;
            b.queueFamily = 0xFFFFFFFF;
            cb->InsertBarrier(&b, 1);
            cb->End();
            cb->Submit(0);
            cb->WaitForCompletion();
            device_->DestroyCommandBuffer(cmd);
        }
    }
    pool.tiles_per_row = cfg_.atlas_size / (cfg_.tile_size + 2 * cfg_.border);
    const u32 capacity = pool.tiles_per_row * pool.tiles_per_row;
    pool.pages.assign(capacity, pool_page{});
    for (u32 i = 0; i < capacity; ++i) pool.free_pages.push_back(i);
    pools_.push_back(std::move(pool));
    format_to_pool_[format_value] = u32(pools_.size() - 1);
    return u32(pools_.size() - 1);
}

void virtual_texture_system::release_texture(u32 vt_id) {
    std::lock_guard<std::mutex> g(stream_mutex_);
    auto it = vts_.find(vt_id);
    if (it == vts_.end()) return;
    // 2.7:按池反查回收该 VT 全部槽位(多层多池;位图语义下不再依赖
    // resident_page_to_slot 单槽记录)
    for (auto& pool : pools_) {
        for (u32 slot = 0; slot < pool.pages.size(); ++slot) {
            if (pool.pages[slot].used && pool.pages[slot].vt_id == vt_id) {
                pool.pages[slot].used = false;
                pool.free_pages.push_back(slot);
            }
        }
    }
    free_page_table_subrect(it->second.pages_top, it->second.pt_origin_x, it->second.pt_origin_y);
    vts_.erase(it);
    // 清扫该 VT 的 in-flight 请求与待上传(槽位归还见 update 阶段 2/3 的存活校验)
    for (auto q = stream_queue_.begin(); q != stream_queue_.end();) {
        if (q->first == vt_id) q = stream_queue_.erase(q);
        else ++q;
    }
    for (u64 key : std::vector<u64>(stream_in_flight_.begin(), stream_in_flight_.end())) {
        if (u32(key >> 32) == vt_id) stream_in_flight_.erase(key);
    }
    auto drop = [&](std::vector<ready_upload>& list) {
        for (auto it2 = list.begin(); it2 != list.end();) {
            if (it2->vt_id == vt_id) it2 = list.erase(it2);
            else ++it2;
        }
    };
    drop(upload_ready_);
    drop(upload_pending_);
    stats_.registered = u32(vts_.size());
}

ResourceHandle virtual_texture_system::atlas_texture(u32 format_slot) const {
    if (format_slot < pools_.size()) return pools_[format_slot].atlas;
    return 0;
}

ResourceHandle virtual_texture_system::feedback_buffer(u32 frame_index) {
    u32 slot = frame_index % feedback_.size();
    feedback_[slot].submitted = false;
    return feedback_[slot].buffer;
}

void virtual_texture_system::record_feedback_copy(RHICommandBuffer* cb, u32 frame_index,
                                                  QueueSubmitInfo& si) {
    if (!cb || !device_) return;
    const u32 slot = frame_index % feedback_.size();
    cb->CopyBuffer(feedback_[slot].buffer, readback_ring_.buffer(slot), 0, 0,
                   u64(cfg_.feedback_slots) * sizeof(u32));
    readback_ring_.attach_fence(si, slot);
    feedback_[slot].submitted = true;
}

void virtual_texture_system::prefetch_page(u32 vt_id, u32 mip, u32 px, u32 py) {
    std::lock_guard<std::mutex> g(stream_mutex_);
    auto it = vts_.find(vt_id);
    if (it == vts_.end()) return;
    const vt_entry& vt = it->second;
    if (mip >= vt.asset.header().mip_count) return;
    const u32 s = vt.pages_top >> mip;
    if (px >= s || py >= s) return;
    const u32 page = mip_page_linear(vt.pages_top, mip, px, py);
    if (page >= vt.page_layer_bits.size()) return;
    const u8 fullMask = u8((1u << vt.layer_count) - 1u);
    if (vt.page_layer_bits[page] == fullMask) {
        ++stats_.producer_cache_hits;   // M5:请求即命中(免生产)
        return;
    }
    submit_streaming(vt_id, page);
}

void virtual_texture_system::prefetch_rect(u32 vt_id, float cx, float cy, float r) {
    std::lock_guard<std::mutex> g(stream_mutex_);
    auto it = vts_.find(vt_id);
    if (it == vts_.end()) return;
    const vt_entry& vt = it->second;
    const float x0 = std::clamp(cx - r, 0.0f, 1.0f);
    const float x1 = std::clamp(cx + r, 0.0f, 1.0f);
    const float y0 = std::clamp(cy - r, 0.0f, 1.0f);
    const float y1 = std::clamp(cy + r, 0.0f, 1.0f);
    const u32 s = vt.pages_top;
    for (u32 py = u32(y0 * s); py <= u32(y1 * s) && py < s; ++py)
        for (u32 px = u32(x0 * s); px <= u32(x1 * s) && px < s; ++px) {
            const u32 page = mip_page_linear(vt.pages_top, 0, px, py);
            if (page >= vt.page_layer_bits.size()) continue;
            const u8 fullMask = u8((1u << vt.layer_count) - 1u);
            if (vt.page_layer_bits[page] == fullMask) {
                ++stats_.producer_cache_hits;   // M5:请求即命中
                continue;
            }
            submit_streaming(vt_id, page);
        }
}

void virtual_texture_system::record_feedback(RHICommandBuffer* cb, u32 frame_index) {
    if (!cb || !device_) return;
    const u32 slot = frame_index % feedback_.size();
    cb->CopyBuffer(feedback_[slot].buffer, readback_ring_.buffer(slot), 0, 0,
                   u64(cfg_.feedback_slots) * sizeof(u32));
    feedback_[slot].submitted = true;
}

// ============================================================================
// update
// ============================================================================
void virtual_texture_system::update(u32 frame_index) {
    if (!device_) return;
    ++frame_counter_;

    // 1. feedback 回读(非阻塞轮询同槽上一周期)
    const u32 slot = frame_index % feedback_.size();
    if (feedback_[slot].submitted && readback_ring_.wait(device_, slot, 0)) {
        process_feedback(slot);
        feedback_[slot].submitted = false;
    }

    // 1.5 M5:producer VT(RVT)请求 — 同步 GPU 生产(三 pass 直写物理池)
    {
        std::vector<std::pair<u32, u32>> producerReqs;
        {
            std::lock_guard<std::mutex> g(stream_mutex_);
            for (auto it = stream_queue_.begin(); it != stream_queue_.end();) {
                auto vit = vts_.find(it->first);
                if (vit != vts_.end() && vit->second.producer) {
                    producerReqs.push_back(*it);
                    it = stream_queue_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [pvt, page] : producerReqs) {
            std::lock_guard<std::mutex> g(stream_mutex_);
            auto vit = vts_.find(pvt);
            if (vit == vts_.end()) { stream_in_flight_.erase((u64(pvt) << 32) | page); continue; }
            vt_entry& vt = vit->second;
            if (page >= vt.page_layer_bits.size()) { stream_in_flight_.erase((u64(pvt) << 32) | page); continue; }
            const u8 fullMask = u8((1u << vt.layer_count) - 1u);
            if (vt.page_layer_bits[page] == fullMask) {
                ++stats_.producer_cache_hits;   // 已驻留(免生产)
                stream_in_flight_.erase((u64(pvt) << 32) | page);
                continue;
            }
            const u32 poolIdx = ensure_pool(vt.layer_formats[0]);
            if (poolIdx == ~0u) { stream_in_flight_.erase((u64(pvt) << 32) | page); continue; }
            physical_pool& pool = pools_[poolIdx];
            if (pool.free_pages.empty()) {
                evict(1);
                if (pool.free_pages.empty()) {
                    stream_in_flight_.erase((u64(pvt) << 32) | page);
                    continue;
                }
            }
            const u32 pageSlot = pool.free_pages.back();
            pool.free_pages.pop_back();
            pool.pages[pageSlot].used = true;
            pool.pages[pageSlot].vt_id = pvt;
            pool.pages[pageSlot].page = page;
            pool.pages[pageSlot].last_use_frame = frame_counter_;
            const u32 ax = (pageSlot % pool.tiles_per_row) * (cfg_.tile_size + 2 * cfg_.border);
            const u32 ay = (pageSlot / pool.tiles_per_row) * (cfg_.tile_size + 2 * cfg_.border);
            // 逐层生产(GPU 三 pass,同步完成)
            bool allLayers = true;
            for (u32 l = 0; l < vt.layer_count; ++l) {
                if (!vt.producer->produce_page(device_, pvt, page, l, pool.atlas, ax, ay)) {
                    allLayers = false;
                    break;
                }
            }
            if (!allLayers) {
                pool.pages[pageSlot].used = false;
                pool.free_pages.push_back(pageSlot);
                stream_in_flight_.erase((u64(pvt) << 32) | page);
                continue;
            }
            // 立即置位 — 同批重复请求(队列多条同页)不再重复生产
            vt.page_layer_bits[page] = fullMask;
            vt.page_layer_origins[page * 4 + 0] = ax | (ay << 16);
            vt.resident_page_to_slot[page] = poolIdx;
            ++vt.resident_count;
            // 构造 gpu_direct 的 ready_upload(跳过 staging,直接标完成)
            ready_upload up{};
            up.vt_id = pvt;
            up.page = page;
            up.layer = 0;
            up.gpu_direct = true;
            up.format = vt.layer_formats[0];
            up.pool_index = poolIdx;
            up.atlas_x = ax;
            up.atlas_y = ay;
            up.data_size = 0;
            upload_ready_.push_back(std::move(up));
            ++stats_.producer_pages_produced;
        }
    }

    // 2. worker 完成的解压 → 分配物理槽 → upload_pending
    {
        std::lock_guard<std::mutex> g(stream_mutex_);
        for (auto& up : upload_ready_) {
            if (up.gpu_direct) {
                upload_pending_.push_back(std::move(up));
                continue;
            }
            auto vtIt = vts_.find(up.vt_id);
            if (vtIt == vts_.end()) {
                // VT 已释放:丢弃(槽位未分配,无泄漏)
                stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
                continue;
            }
            // 单层 VT 的兼容:layer 0
            if (up.layer >= vtIt->second.layer_count) up.layer = 0;
            const u32 poolIdx = ensure_pool(up.format);
            if (poolIdx == ~0u) {
                stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
                continue;
            }
            physical_pool& pool = pools_[poolIdx];
            if (pool.free_pages.empty()) {
                evict(1);
                if (pool.free_pages.empty()) {
                    stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
                    continue;
                }
            }
            u32 pageSlot = pool.free_pages.back();
            pool.free_pages.pop_back();
            pool.pages[pageSlot].used = true;
            pool.pages[pageSlot].vt_id = up.vt_id;
            pool.pages[pageSlot].page = up.page;
            pool.pages[pageSlot].last_use_frame = frame_counter_;
            up.pool_index = poolIdx;
            up.atlas_x = (pageSlot % pool.tiles_per_row) * (cfg_.tile_size + 2 * cfg_.border);
            up.atlas_y = (pageSlot / pool.tiles_per_row) * (cfg_.tile_size + 2 * cfg_.border);
            upload_pending_.push_back(std::move(up));
        }
        upload_ready_.clear();
    }

    // 3. 上传(节流)+ 页表项收集
    stats_.uploads_this_frame = 0;
    while (!upload_pending_.empty() && stats_.uploads_this_frame < cfg_.max_uploads_per_frame) {
        ready_upload up = std::move(upload_pending_.front());
        upload_pending_.erase(upload_pending_.begin());
        if (up.gpu_direct) {
            // M5:GPU 三 pass 已直写 atlas — 仅做完成记账 + 页表条目
            // (bits 已在阶段 1.5 立即置位;此处只补页表条目)
            auto it2 = vts_.find(up.vt_id);
            if (it2 != vts_.end() && up.page < it2->second.page_layer_bits.size()) {
                it2->second.page_layer_bits[up.page] |= u8((1u << it2->second.layer_count) - 1u);
                it2->second.page_layer_origins[up.page * 4 + 0] =
                    up.atlas_x | (up.atlas_y << 16);
                u32 base2 = 0, s2 = it2->second.pages_top, mip2 = 0;
                while (base2 + s2 * s2 <= up.page) { base2 += s2 * s2; s2 >>= 1; ++mip2; }
                const u32 px2 = (up.page - base2) % s2;
                const u32 py2 = (up.page - base2) / s2;
                pt_entries_.push_back(it2->second.pt_origin_x + px2);
                pt_entries_.push_back(it2->second.pt_origin_y + py2);
                for (u32 l = 0; l < 4; ++l) {
                    const u32 o = (l == 0)
                        ? (up.atlas_x | (up.atlas_y << 16)) : 0u;
                    pt_entries_.push_back(l < it2->second.layer_count
                        ? pack_pt_entry(o & 0xFFFF, o >> 16) : 0);
                }
                it2->second.resident_page_to_slot[up.page] = up.pool_index;
            }
            stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
            ++stats_.uploads_this_frame;
            continue;
        }
        {
            std::lock_guard<std::mutex> g(stream_mutex_);
            if (vts_.find(up.vt_id) == vts_.end()) {
                // VT 已释放但槽位已分配:归还槽位
                pools_[up.pool_index].pages[
                    (up.atlas_x / (cfg_.tile_size + 2 * cfg_.border)) +
                    (up.atlas_y / (cfg_.tile_size + 2 * cfg_.border)) *
                        pools_[up.pool_index].tiles_per_row].used = false;
                pools_[up.pool_index].free_pages.push_back(
                    (up.atlas_x / (cfg_.tile_size + 2 * cfg_.border)) +
                    (up.atlas_y / (cfg_.tile_size + 2 * cfg_.border)) *
                        pools_[up.pool_index].tiles_per_row);
                stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
                continue;
            }
        }
        physical_pool& pool = pools_[up.pool_index];

        BufferDesc stagingDesc{};
        stagingDesc.size = up.data_size;
        stagingDesc.type = BufferType::Raw;
        stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        stagingDesc.name = "VTSys_UploadStaging";
        ResourceHandle staging = device_->CreateBuffer(stagingDesc);
        if (staging == handles::INVALID_RESOURCE) break;
        device_->UpdateBufferData(staging, up.data.data(), up.data_size, 0);

        CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
        if (!cb) {
            device_->DestroyBuffer(staging);
            device_->DestroyCommandBuffer(cmd);
            break;
        }
        cb->Reset();
        cb->Begin();
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {int32_t(up.atlas_x), int32_t(up.atlas_y), 0};
        region.imageExtent = {cfg_.tile_size + 2 * cfg_.border,
                              cfg_.tile_size + 2 * cfg_.border, 1};
        cb->CopyBufferToTexture(staging, pool.atlas, &region, 1);
        ResourceBarrier b{};
        b.resource = pool.atlas;
        b.beforeState = ResourceState::CopyDest;
        b.afterState = ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        cb->InsertBarrier(&b, 1);
        cb->End();
        cb->Submit(0);
        cb->WaitForCompletion();   // M2 简化;M6 批合并 + fence 化
        device_->DestroyCommandBuffer(cmd);
        device_->DestroyBuffer(staging);
        ++stats_.uploads_this_frame;

        auto it = vts_.find(up.vt_id);
        if (it != vts_.end() && up.page < it->second.page_layer_bits.size()) {
            // 2.7:层完成位;全层齐才写页表(RGBA 四通道一次写)并记驻留
            it->second.page_layer_bits[up.page] |= u8(1u << up.layer);
            it->second.page_layer_origins[up.page * 4 + up.layer] =
                up.atlas_x | (up.atlas_y << 16);
            const u8 fullMask = u8((1u << it->second.layer_count) - 1u);
            if (it->second.page_layer_bits[up.page] == fullMask) {
                u32 base = 0, s = it->second.pages_top, mip = 0;
                while (base + s * s <= up.page) { base += s * s; s >>= 1; ++mip; }
                const u32 px = (up.page - base) % s;
                const u32 py = (up.page - base) / s;
                pt_entries_.push_back(it->second.pt_origin_x + px);
                pt_entries_.push_back(it->second.pt_origin_y + py);
                // RGBA 四通道:每层各自的物理 origin(层齐后一次写)
                for (u32 l = 0; l < 4; ++l) {
                    const u32 o = it->second.page_layer_origins[up.page * 4 + l];
                    pt_entries_.push_back(l < it->second.layer_count
                        ? pack_pt_entry(o & 0xFFFF, o >> 16) : 0);
                }
                it->second.resident_page_to_slot[up.page] = up.pool_index;
                ++it->second.resident_count;
            }
        }
        stream_in_flight_.erase((u64(up.vt_id) << 32) | up.page);
    }

    // 4. 页表 CS 更新
    if (!pt_entries_.empty()) {
        // 2.7:条目 6 元组 (ptX, ptY, vR, vG, vB, vA) — RGBA 四通道一次写
        std::vector<u32> upload;
        upload.reserve(pt_entries_.size() + 1);
        upload.push_back(u32(pt_entries_.size() / 6));
        upload.insert(upload.end(), pt_entries_.begin(), pt_entries_.end());
        if (pt_entry_buffer_ == 0) {
            BufferDesc d{};
            d.size = 1u << 16;
            d.type = BufferType::Raw;
            d.memoryUsage = GPUMemoryUsage::Dynamic;
            d.name = "VTSys_PTEntries";
            pt_entry_buffer_ = device_->CreateBuffer(d);
        }
        if (pt_entry_buffer_ != 0 &&
            device_->UpdateBufferData(pt_entry_buffer_, upload.data(), upload.size() * 4, 0)) {
            if (!pt_update_pipeline_) {
                // 依平台创建 shader
                ShaderHandle shader = 0;
                if (device_->GetPlatform() == RHIPlatform::Vulkan) {
                    std::ifstream f(kPTUpdateGLSLPath, std::ios::binary | std::ios::ate);
                    if (f) {
                        auto sz = f.tellg();
                        f.seekg(0);
                        std::vector<char> spv(size_t(sz), 0);
                        f.read(spv.data(), sz);
                        shader = device_->CreateShader(spv.data(), spv.size(),
                                                       ShaderStage::Compute, "main");
                    }
                } else if (device_->GetPlatform() == RHIPlatform::Metal) {
                    shader = device_->CreateShader(kPTUpdateMSL, std::strlen(kPTUpdateMSL),
                                                   ShaderStage::Compute, "vtSysUpdatePageTable");
                }
                if (shader) {
                    pt_update_shader_ = shader;
                    DescriptorSetLayoutBinding bind[2]{};
                    bind[0].binding = 0; bind[0].descriptorType = DescriptorType::StorageImage;
                    bind[0].descriptorCount = 1; bind[0].stageFlags = ShaderStage::Compute;
                    bind[1].binding = 1; bind[1].descriptorType = DescriptorType::StorageBuffer;
                    bind[1].descriptorCount = 1; bind[1].stageFlags = ShaderStage::Compute;
                    DescriptorSetLayoutDesc dsl{};
                    dsl.bindingCount = 2;
                    dsl.bindings = bind;
                    pt_update_ds_layout_ = device_->CreateDescriptorSetLayout(dsl);
                    PipelineLayoutDesc pl{};
                    pl.setLayoutCount = 1;
                    pl.setLayouts = &pt_update_ds_layout_;
                    pl.pushConstantRangeCount = 0;
                    pt_update_layout_ = device_->CreatePipelineLayout(pl);
                    DescriptorSetDesc dsd{};
                    dsd.layout = pt_update_ds_layout_;
                    pt_update_ds_ = device_->CreateDescriptorSet(dsd);
                    DescriptorImageInfo ptInfo{};
                    ptInfo.imageView = page_table_;
                    ptInfo.imageLayout = ResourceState::UnorderedAccess;
                    DescriptorBufferInfo eInfo{pt_entry_buffer_, 0, 0};
                    WriteDescriptorSet w[2]{};
                    w[0].dstSet = pt_update_ds_; w[0].dstBinding = 0; w[0].dstArrayElement = 0;
                    w[0].descriptorCount = 1; w[0].descriptorType = DescriptorType::StorageImage;
                    w[0].imageInfo = &ptInfo;
                    w[1].dstSet = pt_update_ds_; w[1].dstBinding = 1; w[1].dstArrayElement = 0;
                    w[1].descriptorCount = 1; w[1].descriptorType = DescriptorType::StorageBuffer;
                    w[1].bufferInfo = &eInfo;
                    device_->UpdateDescriptorSets(2, w);
                    ComputePipelineDesc cpd{};
                    cpd.computeShader = pt_update_shader_;
                    cpd.layout = pt_update_layout_;
                    cpd.threadGroupSize = {64, 1, 1};
                    pt_update_pipeline_ = device_->CreateComputePipeline(cpd);
                    if (pt_update_pipeline_ == handles::INVALID_PIPELINE) {
                        std::cerr << "[VTSys] 页表更新管线创建失败(shader/格式不匹配?)"
                                  << std::endl;
                    }
                }
            }
            if (pt_update_pipeline_ == handles::INVALID_PIPELINE) {
                std::cerr << "[VTSys] 页表更新被跳过(管线无效)— 条目数 "
                          << pt_entries_.size() / 6 << std::endl;
            }
            if (pt_update_pipeline_) {
                CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                RHICommandBuffer* cb = rhi::GetCommandBuffer(cmd);
                if (cb) {
                    cb->Reset();
                    cb->Begin();
                    ResourceBarrier b{};
                    b.resource = page_table_;
                    b.beforeState = ResourceState::ShaderResource;
                    b.afterState = ResourceState::UnorderedAccess;
                    b.subresource = 0xFFFFFFFF;
                    b.queueFamily = 0xFFFFFFFF;
                    cb->InsertBarrier(&b, 1);
                    cb->BindComputePipeline(pt_update_pipeline_);
                    cb->BindDescriptorSets(PipelineBindPoint::Compute, pt_update_layout_,
                                           0, 1, &pt_update_ds_, 0, nullptr);
                    cb->Dispatch((u32(upload.size() / 6) + 63) / 64, 1, 1);
                    ResourceBarrier b2{};
                    b2.resource = page_table_;
                    b2.beforeState = ResourceState::UnorderedAccess;
                    b2.afterState = ResourceState::ShaderResource;
                    b2.subresource = 0xFFFFFFFF;
                    b2.queueFamily = 0xFFFFFFFF;
                    cb->InsertBarrier(&b2, 1);
                    cb->End();
                    cb->Submit(0);
                    cb->WaitForCompletion();
                    device_->DestroyCommandBuffer(cmd);
                }
            }
        }
        pt_entries_.clear();
    }

    // 5. 统计
    stats_.pending_requests = u32(stream_queue_.size() + stream_in_flight_.size());
    u32 used = 0, total = 0, resident = 0;
    for (const auto& pool : pools_) {
        for (const auto& p : pool.pages)
            if (p.used) ++used;
        total += u32(pool.pages.size());
    }
    for (const auto& [id, vt] : vts_) resident += u32(vt.resident_count);
    stats_.pool_pages_used = used;
    stats_.pool_pages_total = total;
    stats_.resident_pages = resident;
}

void virtual_texture_system::process_feedback(u32 slot) {
    const u32* mapped = static_cast<const u32*>(readback_ring_.map(device_, slot));
    if (!mapped) return;
    std::unordered_set<u32> unique;
    for (u32 i = 0; i < cfg_.feedback_slots; ++i) {
        const u32 v = mapped[i];
        if (v == 0) continue;
        unique.insert(v);
    }
    readback_ring_.unmap(device_, slot);
    stats_.feedback_processed += unique.size();

    for (u32 v : unique) {
        u32 vt_id, mip, px, py;
        unpack_feedback(v, vt_id, mip, px, py);
        std::lock_guard<std::mutex> g(stream_mutex_);
        auto it = vts_.find(vt_id);
        if (it == vts_.end()) continue;
        vt_entry& vt = it->second;
        if (mip >= vt.asset.header().mip_count) continue;
        const u32 s = vt.pages_top >> mip;
        if (px >= s || py >= s) continue;
        const u32 page = mip_page_linear(vt.pages_top, mip, px, py);
        if (page >= vt.page_layer_bits.size()) continue;
        const u8 fullMask = u8((1u << vt.layer_count) - 1u);
        if (vt.page_layer_bits[page] == fullMask) {
            // LRU touch
            for (auto& pool : pools_) {
                u32 ps = vt.resident_page_to_slot[page];
                if (ps < pool.pages.size() && pool.pages[ps].used &&
                    pool.pages[ps].vt_id == vt_id) {
                    pool.pages[ps].last_use_frame = frame_counter_;
                    break;
                }
            }
            continue;
        }
        submit_streaming(vt_id, page);
    }
}

void virtual_texture_system::submit_streaming(u32 vt_id, u32 page) {
    const u64 key = (u64(vt_id) << 32) | page;
    if (stream_in_flight_.count(key)) return;
    stream_in_flight_.insert(key);
    stream_queue_.push_back({vt_id, page});
    stream_cv_.notify_one();
}

void virtual_texture_system::evict(u32 needed) {
    for (u32 n = 0; n < needed; ++n) {
        u64 best = ~0ull;
        physical_pool* bestPool = nullptr;
        u32 bestSlot = 0;
        for (auto& pool : pools_) {
            for (u32 s = 0; s < pool.pages.size(); ++s) {
                const pool_page& p = pool.pages[s];
                if (!p.used) continue;
                auto it = vts_.find(p.vt_id);
                if (it != vts_.end() && p.page == total_page_count(it->second.pages_top) - 1)
                    continue;   // root pin
                if (p.last_use_frame < best) {
                    best = p.last_use_frame;
                    bestPool = &pool;
                    bestSlot = s;
                }
            }
        }
        if (!bestPool) return;
        pool_page& victim = bestPool->pages[bestSlot];
        auto it = vts_.find(victim.vt_id);
        if (it != vts_.end()) {
            it->second.resident_page_to_slot[victim.page] = ~0u;
            if (it->second.resident_count > 0) --it->second.resident_count;
            u32 base = 0, s = it->second.pages_top, mip = 0;
            while (base + s * s <= victim.page) { base += s * s; s >>= 1; ++mip; }
            const u32 px = (victim.page - base) % s;
            const u32 py = (victim.page - base) / s;
            pt_entries_.push_back(it->second.pt_origin_x + px);
            pt_entries_.push_back(it->second.pt_origin_y + py);
            pt_entries_.push_back(0);
            pt_entries_.push_back(0);
            pt_entries_.push_back(0);
            pt_entries_.push_back(0);
            it->second.page_layer_bits[victim.page] = 0;
        }
        victim.used = false;
        bestPool->free_pages.push_back(bestSlot);
        ++stats_.evictions_total;
    }
}

u32 virtual_texture_system::vt_pages_top(u32 vt_id) const {
    auto it = vts_.find(vt_id);
    return it == vts_.end() ? 0 : it->second.pages_top;
}
u32 virtual_texture_system::vt_pt_origin_x(u32 vt_id) const {
    auto it = vts_.find(vt_id);
    return it == vts_.end() ? 0 : it->second.pt_origin_x;
}
u32 virtual_texture_system::vt_pt_origin_y(u32 vt_id) const {
    auto it = vts_.find(vt_id);
    return it == vts_.end() ? 0 : it->second.pt_origin_y;
}
u32 virtual_texture_system::vt_avg_color(u32 vt_id) const {
    auto it = vts_.find(vt_id);
    return it == vts_.end() ? 0u : it->second.asset.header().avg_color;
}

} // namespace primal::graphics::vt
