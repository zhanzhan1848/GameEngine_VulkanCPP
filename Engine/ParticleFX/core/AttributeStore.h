#pragma once

// AttributeStore.h — PFX 数据模型：SoA 属性表（架构文档 §3.1）。
// 标准通道固定占 id [0, attr::standard_count)，行内打包与旧
// Particles::particle_data 逐字段一致（兼容桥根基，TestParticleFX 锁定）。
// 行生命周期：free 触发 death 事件（若挂接 event_bus）。
// N0 为单线程实现（系统单点 tick）；JobSystem 并行更新在 N1+ 引入时再议锁策略。

#include "../PfxTypes.h"
#include "Common/CommonHeaders.h"

#include <memory>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

class event_bus;

// 用户通道描述。stride 会被向上对齐到 16B（SIMD 友好）。
struct attr_desc {
    u32 name_hash{ 0 };
    u32 stride{ attr::standard_stride };
};

class attribute_store {
public:
    attribute_store() = default;
    ~attribute_store();

    DISABLE_COPY_AND_MOVE(attribute_store);

    // capacity 行 × (5 标准 + user_channel_count 用户) 通道。
    // 用户通道上限 = max_channels - standard_count。
    bool initialize(u32 capacity, const attr_desc* user_channels, u32 user_channel_count);
    void shutdown();
    bool is_initialized() const { return _buffer != nullptr; }

    // ---- 行生命周期 -------------------------------------------------------
    // 满（无空闲行）时返回无效句柄。
    particle_handle allocate();
    // 句柄校验失败（已死/越界/代数不符）返回 false，不产生事件。
    bool free(particle_handle handle);
    bool is_alive(particle_handle handle) const;
    // 死/越界/代数不符 → nullptr。
    void* resolve(particle_handle handle, u32 channel_id);

    // ---- 模拟循环的轻量迭代面 ---------------------------------------------
    bool is_row_alive(u32 row) const { return _in_use[row] != 0; }
    particle_handle row_handle(u32 row) const { return particle_handle{ row, _generations[row] }; }
    u32 alive_count() const { return _alive; }
    u32 capacity() const { return _capacity; }
    u32 free_count() const { return _free_top; }

    // ---- 通道 -------------------------------------------------------------
    static constexpr u32 max_channels{ 16 };
    u32 channel_count() const { return _channel_count; }
    const attr_desc& channel(u32 id) const { return _channels[id]; }
    // 用户通道按名查找；未命中返回 u32_invalid_id。
    u32 find_channel(u32 name_hash) const;
    // 标准通道 stride 恒为 16B ⇒ 连续行可按 v4 数组寻址（兼容桥与模拟循环依赖此性质）。
    math::v4* v4_channel(u32 standard_id) {
        return static_cast<math::v4*>(static_cast<void*>(_buffer.get() + _offsets[standard_id]));
    }
    const math::v4* v4_channel(u32 standard_id) const {
        return static_cast<const math::v4*>(static_cast<const void*>(_buffer.get() + _offsets[standard_id]));
    }
    void* channel_base(u32 id) { return _buffer.get() + _offsets[id]; }
    const void* channel_base(u32 id) const { return _buffer.get() + _offsets[id]; }

    void set_event_sink(event_bus* bus, emitter_id owner) {
        _events = bus;
        _owner = owner;
    }

private:
    bool handle_alive(particle_handle handle) const {
        return handle.is_valid() && handle.row < _capacity && _in_use[handle.row] != 0 &&
               _generations[handle.row] == handle.generation;
    }
    // 行内寻址（偏移缓存已含通道基址）。
    void* row_ptr(u32 channel_id, u32 row) {
        return _buffer.get() + _offsets[channel_id] + static_cast<size_t>(row) * _channels[channel_id].stride;
    }

    std::unique_ptr<u8[]> _buffer;
    attr_desc             _channels[max_channels]{};
    size_t                _offsets[max_channels]{};
    u32                   _channel_count{ 0 };
    u32                   _capacity{ 0 };
    u32                   _alive{ 0 };
    emitter_id            _owner{ invalid_emitter };

    std::unique_ptr<u32[]> _generations;   // 每槽位；初始 0（从未分配），allocate 自增
    std::unique_ptr<u8[]>  _in_use;
    std::unique_ptr<u32[]> _free_rows;     // LIFO 栈（N0 单线程）
    u32                    _free_top{ 0 };

    event_bus*             _events{ nullptr };
};

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
