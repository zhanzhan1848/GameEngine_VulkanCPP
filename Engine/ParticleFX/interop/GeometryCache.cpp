// GeometryCache.cpp — 见 GeometryCache.h。

#include "GeometryCache.h"

#ifndef DISABLE_PARTICLE_FX

#include <cstdio>
#include <cstring>

namespace primal::pfx::interop {

bool geometry_cache::load(const char* path, std::string& err) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        err = std::string("cannot open '") + path + "'";
        return false;
    }
    char magic[4] = {};
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "PCH1", 4) == 0;
    u32 frame_count = 0, point_count = 0;
    if (ok) ok = std::fread(&frame_count, sizeof(u32), 1, f) == 1;
    if (ok) ok = std::fread(&point_count, sizeof(u32), 1, f) == 1;
    if (!ok || frame_count == 0 || point_count == 0) {
        std::fclose(f);
        err = "bad/truncated pcache header";
        return false;
    }
    _frame_ids.resize(frame_count);
    if (std::fread(_frame_ids.data(), sizeof(u32), frame_count, f) != frame_count) {
        std::fclose(f);
        err = "truncated pcache frame table";
        return false;
    }
    _point_count = point_count;
    _data.resize((size_t)frame_count * point_count * 3);
    if (std::fread(_data.data(), sizeof(f32), _data.size(), f) != _data.size()) {
        std::fclose(f);
        err = "truncated pcache data";
        return false;
    }
    std::fclose(f);
    return true;
}

bool geometry_cache::save(const char* path, std::string& err) const {
    if (_frame_ids.empty()) {
        err = "no frames to save";
        return false;
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        err = std::string("cannot write '") + path + "'";
        return false;
    }
    const u32 frame_count = (u32)_frame_ids.size();
    std::fwrite("PCH1", 1, 4, f);
    std::fwrite(&frame_count, sizeof(u32), 1, f);
    std::fwrite(&_point_count, sizeof(u32), 1, f);
    std::fwrite(_frame_ids.data(), sizeof(u32), frame_count, f);
    std::fwrite(_data.data(), sizeof(f32), _data.size(), f);
    std::fclose(f);
    return true;
}

u32 geometry_cache::add_frame(const f32* xyz, u32 count) {
    if (!_frame_ids.empty() && count != _point_count) return u32_invalid_id;
    if (_point_count == 0) _point_count = count;
    _frame_ids.push_back((u32)_frame_ids.size());
    _data.insert(_data.end(), xyz, xyz + (size_t)count * 3);
    return (u32)_frame_ids.size() - 1;
}

s32 geometry_cache::sample(f32 time, f32 fps, bool loop, math::v3* out_points, u32 cap) const {
    if (_frame_ids.empty() || fps <= 0.f) return 0;
    const f32 frame_f = time * fps;
    s32 idx = (s32)std::floor(frame_f);
    if (loop) idx = ((idx % (s32)_frame_ids.size()) + (s32)_frame_ids.size()) % (s32)_frame_ids.size();
    else if (idx < 0) idx = 0;
    else if (idx >= (s32)_frame_ids.size()) idx = (s32)_frame_ids.size() - 1;

    if (!out_points || cap < _point_count) return -(s32)_point_count;
    const f32* src_data = _data.data() + (size_t)idx * _point_count * 3;
    for (u32 i = 0; i < _point_count; ++i) {
        out_points[i] = math::v3{ src_data[i * 3], src_data[i * 3 + 1], src_data[i * 3 + 2] };
    }
    return (s32)_point_count;
}

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
