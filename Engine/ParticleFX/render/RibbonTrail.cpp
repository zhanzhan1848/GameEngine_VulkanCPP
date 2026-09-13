// RibbonTrail.cpp — 见 RibbonTrail.h。

#include "RibbonTrail.h"

#include <cmath>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

void ribbon_trail::configure(u32 samples, f32 sample_interval, f32 width) {
    if (samples == 0) samples = 1;
    _samples.assign(samples, ribbon_sample{});
    _interval = sample_interval > 0.f ? sample_interval : 1.f / 60.f;
    _width = width > 0.f ? width : 0.1f;
    _count = 0;
    _head = 0;
    _accum = 0.f;
}

void ribbon_trail::attach(particle_handle handle, const attribute_store* store) {
    handle_ = handle;
    store_ = store;
    attached_ = handle.is_valid() && store != nullptr;
    if (!attached_) clear_history();
}

void ribbon_trail::record(f32 dt) {
    if (!attached_) return;
    // death 截断：句柄失效（粒子死亡/行复用）即清历史，拖尾不得悬空
    if (!store_->is_alive(handle_) || !store_->is_row_alive(handle_.row)) {
        clear_history();
        attached_ = false;
        return;
    }
    const math::v4* pos = store_->v4_channel(attr::position) + handle_.row;
    const math::v4* vel = store_->v4_channel(attr::velocity) + handle_.row;
    _accum += dt;
    while (_accum >= _interval) {
        _accum -= _interval;
        ribbon_sample s;
        s.pos = math::v3{ pos->x, pos->y, pos->z };
        s.width = _width;
        s.age_norm = vel->w > 0.f ? (pos->w / vel->w < 1.f ? pos->w / vel->w : 1.f) : 1.f;
        push_sample(s);
    }
}

void ribbon_trail::push_sample(const ribbon_sample& s) {
    _samples[_head] = s;
    _head = (_head + 1) % (u32)_samples.size();
    if (_count < (u32)_samples.size()) ++_count;
}

u32 ribbon_trail::build_strip(math::v3 camera_right, ribbon_vertex* out_verts, u32 vert_cap,
                              u16* out_indices, u32 index_cap) const {
    if (_count < 2 || !out_verts || !out_indices) return 0;
    const u32 needed_verts = _count * 2;
    const u32 needed_idx = (_count - 1) * 6;
    if (vert_cap < needed_verts || index_cap < needed_idx) return 0;

    camera_right = camera_right * (1.f / std::sqrt(camera_right.x * camera_right.x +
                                                  camera_right.y * camera_right.y +
                                                  camera_right.z * camera_right.z));
    // 环形序：最老在前（尾），最新在后（头）
    const u32 start = (_head + (u32)_samples.size() - _count) % (u32)_samples.size();
    for (u32 i = 0; i < _count; ++i) {
        const ribbon_sample& s = _samples[(start + i) % (u32)_samples.size()];
        const f32 t = (f32)i / (f32)(_count - 1);      // 0 尾 → 1 头
        const f32 half = s.width * (1.f - 0.5f * (1.f - t));   // 尾部收窄到 50%
        const f32 hw = half * 0.5f;
        ribbon_vertex& left = out_verts[i * 2];
        ribbon_vertex& right = out_verts[i * 2 + 1];
        left.pos[0] = s.pos.x - camera_right.x * hw;
        left.pos[1] = s.pos.y - camera_right.y * hw;
        left.pos[2] = s.pos.z - camera_right.z * hw;
        right.pos[0] = s.pos.x + camera_right.x * hw;
        right.pos[1] = s.pos.y + camera_right.y * hw;
        right.pos[2] = s.pos.z + camera_right.z * hw;
        left.pad = right.pad = 0.f;
        left.uv[0] = 0.f; left.uv[1] = t;
        right.uv[0] = 1.f; right.uv[1] = t;
        left.reserved[0] = left.reserved[1] = 0.f;
        right.reserved[0] = right.reserved[1] = 0.f;
    }
    for (u32 i = 0; i + 1 < _count; ++i) {
        const u16 v0 = (u16)(i * 2);
        u16 quad[6] = { v0, (u16)(v0 + 1), (u16)(v0 + 2), v0, (u16)(v0 + 2), (u16)(v0 + 3) };
        for (int k = 0; k < 6; ++k) out_indices[i * 6 + k] = quad[k];
    }
    return _count;
}

bool ribbon_trail::head_position(math::v3& out) const {
    if (_count == 0) return false;
    const u32 newest = (_head + (u32)_samples.size() - 1) % (u32)_samples.size();
    out = _samples[newest].pos;
    return true;
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
