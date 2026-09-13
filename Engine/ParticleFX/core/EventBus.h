#pragma once

// EventBus.h — PFX 事件管道（架构文档 §3.5，ADR4）。
// 类型化 POD + 环形缓冲 + 单帧消费：消费者缺席时事件自然丢弃（零积压）。
// 满时丢弃新事件并计数（保确定性——不覆盖旧事件）；
// 丢弃告警限频：首次与每 1024 次各告警一条。

#include "../PfxTypes.h"
#include "Common/CommonHeaders.h"

#include <memory>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

class event_bus {
public:
    event_bus() = default;
    ~event_bus() { shutdown(); }

    DISABLE_COPY_AND_MOVE(event_bus);

    bool initialize(u32 capacity);
    void shutdown();
    bool is_initialized() const { return _ring != nullptr; }

    void push(const pfx_event& e);
    // 取出至多 cap 条（FIFO），返回实际取出数。
    u32 drain(pfx_event* out, u32 cap);

    u32 pending() const { return _count; }
    u32 capacity() const { return _capacity; }
    u64 dropped_total() const { return _dropped; }

private:
    std::unique_ptr<pfx_event[]> _ring;
    u32 _capacity{ 0 };
    u32 _head{ 0 };   // 下一条可读位置
    u32 _count{ 0 };
    u64 _dropped{ 0 };
};

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
