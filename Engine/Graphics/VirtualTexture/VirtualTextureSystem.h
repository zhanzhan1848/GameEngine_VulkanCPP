/**
 * @file VirtualTextureSystem.h
 * @brief VT M2 — SVT 运行时核心(页表空间 + 物理页池 + feedback + 流送)
 * @details RHI 之上的能力模块(设计文档 §3/§5)。M2 范围:
 *   - 全局单例 virtual_texture_system:VTEX 注册 → 页表子矩形分配 →
 *     feedback 驱动的按需加载 → 物理页池上传 → 页表 CS 更新;
 *   - 页表:单层 256x256 R32_UINT,mip-金字塔打包布局(每个 VT 的 mip L 页
 *     占其子矩形左上 (pages>>L)² 区域 — 采样回退 = 坐标右移,无需 mip 链);
 *   - 条目位布局(与 M0 原型一致):bit0 valid / [8:19) atlasOriginX /
 *     [19:30) atlasOriginY;
 *   - 物理页池:按格式分组,2048² atlas,LRU 二叉堆淘汰,root 页 pin;
 *   - feedback:N 帧轮转 SSBO → rhi_readback_ring 回读 → CPU 去重分析;
 *   - 流送:内存驻留 blob + zlib 解压(worker 线程)→ 上传队列(压缩格式走
 *     命令层 CopyBufferToTexture,非压缩走 UpdateTextureRegion)。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-22
 */

#pragma once

#include "../../Content/VTEXAsset.h"
#include "../RHI/Core/RHITypes.h"
#include "../RHI/Core/RHIReadbackRing.h"
#include "../RHI/Core/RHICommand.h"

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace primal::graphics::vt {

// RHI 类型直接进入 vt 命名空间(ResourceHandle/RHIDeviceBase/RHICommandBuffer 等)
using namespace primal::graphics::rhi;

// ============================================================================
// 配置(cvar 化属 M6;先静态可改)
// ============================================================================
struct vt_config {
    u32 tile_size          = 128;
    u32 border             = 4;
    u32 page_table_size    = 256;   // 页表边长(texel)
    u32 atlas_size         = 2048;  // 每格式组物理池边长
    u32 max_uploads_per_frame = 8;  // 上传节流
    u32 page_free_threshold = 15;   // 淘汰触发:空闲页低于此值(%容量近似为页数)
    u32 feedback_slots     = 1 << 20;  // feedback 条目数(1M)
    u32 feedback_latency   = 2;     // 回读延迟帧数
};

// 采样参数(shader 侧 VTSys_Params 镜像)
struct vt_sample_params {
    u32 vt_id;
    u32 pages_top;       // VT 顶层 mip 页格边长
    u32 out_width;       // 输出网格边长
    u32 level;           // 采样 mip(测试用;材质路径用导数)
};

// 统计(2.18)
struct vt_stats {
    u32 registered       = 0;
    u32 resident_pages   = 0;
    u32 pending_requests = 0;
    u32 uploads_this_frame = 0;
    u64 feedback_processed = 0;
    u32 pool_pages_used  = 0;
    u32 pool_pages_total = 0;
    u32 evictions_total  = 0;
    u32 producer_pages_produced = 0;   // M5:RVT 页生产次数
    u32 producer_cache_hits      = 0;   // M5:请求时已驻留(免生产)
};

// feedback 32bit 编码:|4bit vtId|4bit mip|12bit vPageX|12bit vPageY|
inline u32 pack_feedback(u32 vt_id, u32 mip, u32 px, u32 py) {
    return (vt_id << 28) | (mip << 24) | ((py & 0xFFF) << 12) | (px & 0xFFF);
}
inline void unpack_feedback(u32 v, u32& vt_id, u32& mip, u32& px, u32& py) {
    vt_id = v >> 28; mip = (v >> 24) & 0xF; px = v & 0xFFF; py = (v >> 12) & 0xFFF;
}

// 页表条目位布局(同 M0 原型)
inline u32 pack_pt_entry(u32 atlas_x, u32 atlas_y) {
    return 1u | (atlas_x << 8) | (atlas_y << 19);
}

// ============================================================================
// M5:Producer 接口 — 页数据生产者(SVT=磁盘;RVT=GPU 渲染)
// ============================================================================
struct IVTProducer {
    virtual ~IVTProducer() = default;
    /// GPU 直写物理池:系统已分配 atlas 槽位,producer 负责把该页该层的
    /// 内容写进 atlas[atlas_x, atlas_y] 起 136×136 区域(三 pass:
    /// 内容捕获 → 压缩 → 存储镜像直写)。同步完成(返回即就绪)。
    /// 返回 false 则系统取消该请求(不标驻留)。
    virtual bool produce_page(RHIDeviceBase* device, u32 vt_id, u32 page, u32 layer,
                              ResourceHandle atlas, u32 atlas_x, u32 atlas_y) = 0;
};

class virtual_texture_system {
public:
    static virtual_texture_system& instance();

    bool initialize(RHIDeviceBase* device, const vt_config& cfg = vt_config{});
    void shutdown();
    bool is_initialized() const { return device_ != nullptr; }

    /// 注册 VTEX 容器(blob 拷贝由系统持有)。返回 vt_id;失败返回 ~0u。
    u32 register_texture(const u8* vtex_data, u64 vtex_size);

    /// M5:注册运行时 VT(RVT — 无磁盘容器,页由 producer 按需生产)。
    /// width/height 为虚拟分辨率(2^n);format 通常 RGBA8(RVT 池)。
    u32 register_runtime_texture(u32 width, u32 height, u32 format,
                                 IVTProducer* producer);
    void release_texture(u32 vt_id);

    /// 采样绑定资源(页表 / 物理池 atlas;descriptor 用)
    ResourceHandle page_table_texture() const { return page_table_; }
    ResourceHandle atlas_texture(u32 format_slot) const;

    /// 本帧采样 pass 用的 feedback SSBO(绑定到 set,采样函数写入)
    ResourceHandle feedback_buffer(u32 frame_index);

    /// 某帧 feedback ring fence(调用方 submit 时作 signalFence)
    SyncHandle feedback_fence_handle(u32 frame_index) const {
        return readback_ring_.fence(frame_index % feedback_.size());
    }
    /// 仅记录回读拷贝(不构造 si — 配合直接传 signalFence 的提交路径)
    void record_feedback(RHICommandBuffer* cb, u32 frame_index);

    /// 等待某帧 feedback 提交的 fence(测试/工具用;正常管线由 update 轮询)
    bool wait_feedback(u32 frame_index, u32 timeout_ms) {
        return readback_ring_.wait(device_, frame_index % feedback_.size(), timeout_ms);
    }

    /// M3 HybridPrefetcher(UE 5.6 式):CPU 侧主动预取 — 距离/速度驱动的
    /// 页请求入口。与 feedback 路径共用流送队列(优先级可见 > 预取由
    /// 队列 FIFO 保证);幂等(已驻留/在途则忽略)。
    /// mip/px/py 为该 VT 的页坐标(金字塔打包语义同 feedback 编码)。
    void prefetch_page(u32 vt_id, u32 mip, u32 px, u32 py);
    /// 便利:预取 UV 矩形覆盖的全部 mip0 页(相机前方区域)
    void prefetch_rect(u32 vt_id, float uv_center_x, float uv_center_y, float uv_radius);

    /// 帧末在渲染命令缓冲上记录 feedback 回读拷贝,并把 ring fence 挂到提交信息。
    /// 调用方随后 submit si;update() 会在若干帧后轮询到就绪并处理。
    void record_feedback_copy(RHICommandBuffer* cb, u32 frame_index, QueueSubmitInfo& si);

    /// 帧驱动:处理 feedback → 流送 → 上传 → 页表更新。
    /// 在渲染 submit 之后、下一帧开始之前调用(帧号任意递增)。
    void update(u32 frame_index);

    const vt_stats& stats() const { return stats_; }
    const vt_config& config() const { return cfg_; }

    /// VT 元信息(测试/调试)
    u32 vt_pages_top(u32 vt_id) const;
    u32 vt_pt_origin_x(u32 vt_id) const;
    u32 vt_pt_origin_y(u32 vt_id) const;
    u32 vt_avg_color(u32 vt_id) const;

private:
    virtual_texture_system() = default;
    ~virtual_texture_system() { shutdown(); }
    virtual_texture_system(const virtual_texture_system&) = delete;
    virtual_texture_system& operator=(const virtual_texture_system&) = delete;

    struct vt_entry {
        u32 id = 0;
        std::vector<u8> blob;
        primal::content::vtex_asset asset;
        u32 pt_origin_x = 0, pt_origin_y = 0;  // 页表子矩形原点
        u32 pages_top = 0;                     // 顶层 mip 页格边长
        // 页驻留状态:线性 page = mip 页表坐标(金字塔打包)相对原点
        std::vector<u32> resident_page_to_slot;  // 页 → 物理池槽位(~0u 未驻留)
        u64 resident_count = 0;
        // 2.7 多层:层格式(平台选定段)与 per-page 层完成位(L 位全 1 = 驻留)
        u32 layer_count = 1;
        u32 layer_formats[4] = {0, 0, 0, 0};
        IVTProducer* producer = nullptr;   // M5:非空 = 页请求路由到 GPU 生产
        std::vector<u8> page_layer_bits;        // 每页一个位掩码
        std::vector<u32> page_layer_origins;    // 每页 4 槽:层 l 的 atlas origin(x|y<<16)
    };

    struct pool_page {
        u64 last_use_frame = 0;
        u32 vt_id = 0;
        u32 page = 0;      // vt_entry 内线性页索引
        bool used = false;
    };

    struct physical_pool {
        u32 format = 0;                 // DataFormat 值
        ResourceHandle atlas = 0;
        std::vector<pool_page> pages;   // 槽位 → 占用
        std::vector<u32> free_pages;    // 空闲槽位栈
        u32 tiles_per_row = 0;          // atlas 内 tile 网格
    };

    // --- 子矩形分配(2 的幂方块;简化 buddy:按尺寸分桶的自由方块表) ---
    bool allocate_page_table_subrect(u32 pages_top, u32& ox, u32& oy);
    void free_page_table_subrect(u32 pages_top, u32 ox, u32 oy);
    u32 mip_page_linear(u32 pages_top, u32 mip, u32 px, u32 py) const {
        // 金字塔打包:mip L 网格边长 s = pages_top >> L;线性 = sum(前面层) + y*s + x
        u32 base = 0, s = pages_top;
        for (u32 l = 0; l < mip; ++l) { base += s * s; s >>= 1; }
        return base + py * s + px;
    }
    u32 total_page_count(u32 pages_top) const {
        u32 total = 0, s = pages_top;
        while (s) { total += s * s; s >>= 1; }
        return total;
    }

    // --- feedback / 流送 ---
    void process_feedback(u32 slot);
    void submit_streaming(u32 vt_id, u32 page);
    void evict(u32 needed);
    u32 ensure_pool(u32 format_value);   // 懒建物理池,返回 pool index

    RHIDeviceBase* device_ = nullptr;
    vt_config cfg_{};
    vt_stats stats_{};

    std::unordered_map<u32, vt_entry> vts_;
    u32 next_vt_id_ = 1;             // 兼容保留;实际分配改为扫描空闲 id

    ResourceHandle page_table_ = 0;
    // 子矩形分配器:尺寸(2^n)→ 已分配原点集合 + 空闲方块(简单贪婪)
    std::unordered_map<u32, std::vector<std::pair<u32, u32>>> free_subrects_;

    std::vector<physical_pool> pools_;              // 按格式
    std::unordered_map<u32, u32> format_to_pool_;   // DataFormat → pool index

    // feedback:每 latency 槽一个 SSBO + ring 回读
    struct feedback_slot {
        ResourceHandle buffer = 0;
        bool submitted = false;
        u64 request_count = 0;
    };
    std::vector<feedback_slot> feedback_;
    rhi_readback_ring readback_ring_;
    u32 feedback_frame_counter_ = 0;

    // 流送 worker
    std::thread worker_;
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
    std::deque<std::pair<u32, u32>> stream_queue_;     // (vt_id, page)
    std::unordered_set<u64> stream_in_flight_;          // key = vt<<32 | page
    struct ready_upload {
        u32 vt_id, page;
        u32 layer = 0;   // 2.7:多层 VT 的层号(页表通道/层池)
        u32 format;
        bool gpu_direct = false;   // M5:GPU 直写已完成,跳过 staging 上传
        std::vector<u8> data;      // 解压后的 tile 数据(压缩或原始)
        u64 data_size;
        u32 atlas_x, atlas_y;      // 已分配的物理槽位 origin
        u32 pool_index;
    };
    std::vector<ready_upload> upload_ready_;           // worker → 主线程
    std::vector<ready_upload> upload_pending_;         // 主线程待上传(分配了槽位)
    bool shutdown_requested_ = false;

    // 页表更新批(本帧收集,上传后一并 CS 写)
    std::vector<u32> pt_entries_;   // (ptX, ptY, value) 三元组平铺
    ResourceHandle pt_entry_buffer_ = 0;
    ResourceHandle pt_update_pipeline_ = 0;
    PipelineLayoutHandle pt_update_layout_ = 0;
    DescriptorSetLayoutHandle pt_update_ds_layout_ = 0;
    DescriptorSetHandle pt_update_ds_ = 0;
    ShaderHandle pt_update_shader_ = 0;

    u64 frame_counter_ = 0;
};

} // namespace primal::graphics::vt
