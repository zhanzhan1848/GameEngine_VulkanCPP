// AttributeStore.cpp — 见 AttributeStore.h。

#include "AttributeStore.h"
#include "EventBus.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

attribute_store::~attribute_store() { shutdown(); }

bool attribute_store::initialize(u32 capacity, const attr_desc* user_channels, u32 user_channel_count) {
    shutdown();

    if (capacity == 0) return false;
    if (attr::standard_count + user_channel_count > max_channels) return false;
    for (u32 i = 0; i < user_channel_count; ++i) {
        if (user_channels[i].stride == 0) return false;
    }

    _capacity = capacity;
    _channel_count = attr::standard_count + user_channel_count;

    // 偏移表：通道为段（channel-major SoA），每段占 stride × capacity 字节。
    // 标准通道（固定 16B）在前，用户通道 stride 向上对齐 16B。
    size_t offset = 0;
    for (u32 i = 0; i < attr::standard_count; ++i) {
        _channels[i] = attr_desc{ 0, attr::standard_stride };
        _offsets[i] = offset;
        offset += static_cast<size_t>(attr::standard_stride) * capacity;
    }
    for (u32 i = 0; i < user_channel_count; ++i) {
        const u32 stride = (user_channels[i].stride + 15u) & ~15u;
        _channels[attr::standard_count + i] = attr_desc{ user_channels[i].name_hash, stride };
        _offsets[attr::standard_count + i] = offset;
        offset += static_cast<size_t>(stride) * capacity;
    }

    _buffer = std::make_unique<u8[]>(offset);
    _generations = std::make_unique<u32[]>(capacity);       // 0 = 从未分配
    _in_use = std::make_unique<u8[]>(capacity);
    _free_rows = std::make_unique<u32[]>(capacity);

    // LIFO 栈顶先弹 —— 倒序填栈使首次分配落在 row 0（行密度优先，缓存友好）。
    for (u32 row = 0; row < capacity; ++row) _free_rows[row] = capacity - 1 - row;
    _free_top = capacity;
    _alive = 0;
    return true;
}

void attribute_store::shutdown() {
    _buffer.reset();
    _generations.reset();
    _in_use.reset();
    _free_rows.reset();
    _channel_count = 0;
    _capacity = 0;
    _alive = 0;
    _free_top = 0;
}

particle_handle attribute_store::allocate() {
    if (_free_top == 0) return particle_handle{};
    const u32 row = _free_rows[--_free_top];
    ++_generations[row];            // 初始 0 → 首次分配为 1；复用必然换代（ADR2）
    _in_use[row] = 1;
    ++_alive;
    return particle_handle{ row, _generations[row] };
}

bool attribute_store::free(particle_handle handle) {
    if (!handle_alive(handle)) return false;

    const u32 row = handle.row;

    // death 事件携带寿命进度；数据仍在缓冲里，本帧内消费者仍可按行寻址读取，
    // 但句柄语义上已死（resolve 拒绝）——ADR2 的时序约定。
    if (_events) {
        const math::v4* pos = v4_channel(attr::position) + row;
        const math::v4* vel = v4_channel(attr::velocity) + row;
        f32 normalized = 1.0f;
        if (vel->w > 0.0f) {
            normalized = pos->w / vel->w;
            normalized = normalized < 0.0f ? 0.0f : (normalized > 1.0f ? 1.0f : normalized);
        }
        _events->push(pfx_event{ particle_handle{ row, handle.generation }, _owner,
                                 event_type::death, normalized });
    }

    _in_use[row] = 0;
    _free_rows[_free_top++] = row;
    --_alive;
    return true;
}

bool attribute_store::is_alive(particle_handle handle) const { return handle_alive(handle); }

void* attribute_store::resolve(particle_handle handle, u32 channel_id) {
    if (!handle_alive(handle) || channel_id >= _channel_count) return nullptr;
    return row_ptr(channel_id, handle.row);
}

u32 attribute_store::find_channel(u32 name_hash) const {
    for (u32 i = attr::standard_count; i < _channel_count; ++i) {
        if (_channels[i].name_hash == name_hash) return i;
    }
    return u32_invalid_id;
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
