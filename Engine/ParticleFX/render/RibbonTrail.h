#pragma once

// RibbonTrail.h — ribbon/trail 渲染器 CPU 侧（架构文档 §7-N6）。
//
// 数据流：attach 到一个粒子句柄 → 每帧 record() 按固定时间间隔采样位置进环形
// 缓冲（粒子死亡经 resolve 失败自动截断——§7-N6 "death 截断"）→ build_strip()
// 生成面向相机的条带几何（顶点 + u16 索引）→ ParticleFXRibbonPass 绘制。
// 宽度沿尾向线性收窄（曲线挂接为后续增强）。全部纯 CPU、确定性。

#include "../PfxTypes.h"
#include "../core/AttributeStore.h"

#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

// 条带顶点（32B，与 ParticleFXRibbon.metal 的 RibbonPackedVertex 对齐）
struct ribbon_vertex {
    f32 pos[3];
    f32 pad;
    f32 uv[2];      // x: 横向 [0,1]，y: 纵向（尾 0 → 头 1）
    f32 reserved[2];
};
static_assert(sizeof(ribbon_vertex) == 32, "ribbon vertex layout locked (32B)");

struct ribbon_sample {
    math::v3 pos{};
    f32 width{ 0.1f };
    f32 age_norm{ 0.f };
};

class ribbon_trail {
public:
    static constexpr u32 kDefaultSamples{ 32 };

    void configure(u32 samples, f32 sample_interval, f32 width);

    // 绑定到粒子（存活期由 store.resolve 语义决定）
    void attach(particle_handle handle, const attribute_store* store);
    void detach() { attached_ = false; }
    bool is_attached() const { return attached_; }

    // 每帧推进：按 sample_interval 累积采样位置。粒子死亡 → 历史清空（截断）。
    void record(f32 dt);

    // 条带几何打包：camera_right 为面向相机的横向单位向量。
    // 返回顶点数；索引数 = max(0, (samples-1)) * 6。容量不足返回 0。
    u32 build_strip(math::v3 camera_right, ribbon_vertex* out_verts, u32 vert_cap,
                    u16* out_indices, u32 index_cap) const;

    // 头端（最新采样）位置；无历史返回 false。
    bool head_position(math::v3& out) const;

    u32 sample_capacity() const { return (u32)_samples.size(); }
    u32 sample_count() const { return _count; }
    void clear_history() { _count = 0; _head = 0; }

private:
    void push_sample(const ribbon_sample& s);

    std::vector<ribbon_sample> _samples;   // 环形缓冲（固定容量）
    u32 _head{ 0 };                         // 下一写入位
    u32 _count{ 0 };                        // 有效样本数
    f32 _interval{ 1.f / 60.f };
    f32 _accum{ 0.f };
    f32 _width{ 0.1f };
    bool attached_{ false };
    particle_handle handle_{};
    const attribute_store* store_{ nullptr };
};

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
