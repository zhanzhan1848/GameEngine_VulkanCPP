// EventBus.cpp — 见 EventBus.h。

#include "EventBus.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

bool event_bus::initialize(u32 capacity) {
    shutdown();
    if (capacity == 0) return false;
    _ring = std::make_unique<pfx_event[]>(capacity);
    _capacity = capacity;
    _head = 0;
    _count = 0;
    _dropped = 0;
    return true;
}

void event_bus::shutdown() {
    _ring.reset();
    _capacity = 0;
    _head = 0;
    _count = 0;
    _dropped = 0;
}

void event_bus::push(const pfx_event& e) {
    if (!_ring) return;
    if (_count == _capacity) {
        ++_dropped;
        // 限频告警：首丢必报，之后每 1024 次报一次（满载时不能刷爆 stderr）。
        if (_dropped == 1 || (_dropped % 1024) == 0) {
            PRIMAL_LOG_STREAM(Warn, "ParticleFX")
                << "event_bus overflow: dropped_total=" << _dropped
                << " (capacity=" << _capacity << ")" << std::endl;
        }
        return;
    }
    const u32 tail = (_head + _count) % _capacity;
    _ring[tail] = e;
    ++_count;
}

u32 event_bus::drain(pfx_event* out, u32 cap) {
    if (!_ring || !out || cap == 0) return 0;
    const u32 n = cap < _count ? cap : _count;
    for (u32 i = 0; i < n; ++i) {
        out[i] = _ring[_head];
        _head = (_head + 1) % _capacity;
    }
    _count -= n;
    return n;
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
