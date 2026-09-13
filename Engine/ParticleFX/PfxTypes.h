#pragma once

// PfxTypes.h — PFX（Primal FX 可编程粒子框架）核心类型。
// 架构文档：Docs/2026-09-12-particle-framework-architecture.md（§3 核心设计）。
// 依赖闭包纪律：本文件只允许依赖 Common/ + Utilities/（松耦合规则 1）。

#include "Common/CommonHeaders.h"

namespace primal::pfx {

// -----------------------------------------------------------------------------
// 粒子句柄（架构 ADR2）
// (row, generation)：generation 在每次 allocate 时自增（初始 1），行复用必然换代。
// 外部一律持句柄不持裸索引；resolve 失败即粒子已死。
// -----------------------------------------------------------------------------

struct particle_handle {
    u32 row{ u32_invalid_id };
    u32 generation{ 0 };

    constexpr bool is_valid() const noexcept { return row != u32_invalid_id && generation != 0; }
};

constexpr inline bool operator==(particle_handle a, particle_handle b) noexcept {
    return a.row == b.row && a.generation == b.generation;
}
constexpr inline bool operator!=(particle_handle a, particle_handle b) noexcept {
    return !(a == b);
}

// -----------------------------------------------------------------------------
// 发射器 id（单调递增、不复用 —— 销毁后残留在事件里的 source id 不会错绑新发射器）
// -----------------------------------------------------------------------------

using emitter_id = u32;
constexpr emitter_id invalid_emitter{ u32_invalid_id };

// -----------------------------------------------------------------------------
// 事件（架构 ADR4：POD + 环形缓冲 + 单帧消费）
// -----------------------------------------------------------------------------

enum class event_type : u8 { birth = 0, death = 1, collision = 2 }; // collision 于 N4 启用

struct pfx_event {
    particle_handle handle;
    emitter_id      source{ invalid_emitter };
    event_type      type{ event_type::birth };
    f32             normalized_age{ 0.f };   // 事件发生时的寿命进度 [0,1]
};

// 布局锁定：环形缓冲按值拷贝 + 未来跨 ABI 读取的前提。
static_assert(sizeof(pfx_event) == 20, "pfx_event 布局被锁定，改动需过 ABI 评审");

// -----------------------------------------------------------------------------
// 标准通道（兼容桥根基）
// 固定占通道 id [0, standard_count)；行内按下标顺序打包，5 × 16B = 80B，
// 与旧 Particles::particle_data 逐字段一致（由 TestParticleFX 锁定）。
// -----------------------------------------------------------------------------

namespace attr {
    constexpr u32 position{ 0 };       // v4: xyz = 世界位置, w = age
    constexpr u32 velocity{ 1 };       // v4: xyz = 速度,     w = lifetime
    constexpr u32 color{ 2 };          // v4: rgba
    constexpr u32 scale_rotation{ 3 }; // v4: xy = scale, zw = rotation（与旧通道名对齐）
    constexpr u32 uv_params{ 4 };      // v4: x = atlas_index, y = frame_progress

    constexpr u32 standard_count{ 5 };
    constexpr u32 standard_stride{ 16 };
} // namespace attr

// -----------------------------------------------------------------------------
// 确定性 RNG（架构 §3.2：每发射器独立 pcg32 流，固定种子 ⇒ 哈希门禁可复现）
// pcg32：O'Neill 公有领域变体（XSH-RR）。
// -----------------------------------------------------------------------------

struct pcg32 {
    u64 state{ 0x853c49e6748fea9bULL };
    u64 inc{ 0xda3e39cb94b95bdbULL };

    void seed(u64 sequence) {
        state = 0u;
        inc = (sequence << 1u) | 1u;
        next_u32();
        state += 0x14057b7ef767814fULL;
        next_u32();
    }

    u32 next_u32() {
        const u64 old = state;
        state = old * 6364136223846793005ULL + inc;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
    }

    f32 next_f32() { return static_cast<f32>(next_u32() >> 8) * (1.0f / 16777216.0f); } // [0,1)
};

// -----------------------------------------------------------------------------
// 名字哈希（模块/属性/暴露参数统一用；Murmur3-32，种子固定）
// -----------------------------------------------------------------------------

inline u32 hash_name(const char* s) {
    u32 out = 0;
    utl::MurmurHash3_x86_32(s, (int)std::strlen(s), 0x9E3779B9u, &out);
    return out;
}

} // namespace primal::pfx
