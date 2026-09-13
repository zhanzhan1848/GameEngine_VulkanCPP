// FieldSystem.cpp — 见 FieldSystem.h。

#include "FieldSystem.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>

namespace {

inline f32 lerp_m(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

} // namespace

#include <cstdio>
#include <cstring>

namespace primal::pfx {

volume_grid::~volume_grid() { shutdown(); }

bool volume_grid::init_scalar(u32 nx, u32 ny, u32 nz, math::v3 origin, f32 cell, f32 fill) {
    shutdown();
    if (nx == 0 || ny == 0 || nz == 0 || cell <= 0.f) return false;
    _kind = kind::scalar;
    _dims[0] = nx;
    _dims[1] = ny;
    _dims[2] = nz;
    _origin = origin;
    _cell = cell;
    _data.assign((size_t)nx * ny * nz, fill);
    return true;
}

bool volume_grid::init_vector(u32 nx, u32 ny, u32 nz, math::v3 origin, f32 cell, math::v3 fill) {
    if (!init_scalar(nx, ny, nz, origin, cell, 0.f)) return false;
    _kind = kind::vector3;
    _data.assign((size_t)nx * ny * nz * 3, 0.f);
    for (size_t i = 0; i < (size_t)nx * ny * nz; ++i) {
        _data[i * 3] = fill.x;
        _data[i * 3 + 1] = fill.y;
        _data[i * 3 + 2] = fill.z;
    }
    return true;
}

void volume_grid::shutdown() {
    _data.clear();
    _dims[0] = _dims[1] = _dims[2] = 0;
    _cell = 1.f;
}

void volume_grid::world_to_grid(math::v3 world, f32& gx, f32& gy, f32& gz) const {
    gx = (world.x - _origin.x) / _cell;
    gy = (world.y - _origin.y) / _cell;
    gz = (world.z - _origin.z) / _cell;
}

f32 volume_grid::sample_scalar(math::v3 world) const {
    f32 gx, gy, gz;
    world_to_grid(world, gx, gy, gz);
    const f32 x0 = std::floor(gx), y0 = std::floor(gy), z0 = std::floor(gz);
    const f32 tx = gx - x0, ty = gy - y0, tz = gz - z0;
    const s32 ix = (s32)x0, iy = (s32)y0, iz = (s32)z0;
    const u32 nx = _dims[0], ny = _dims[1], nz = _dims[2];

    // 界外：SDF 外推值（signed 场的语义）
    if (ix < 0 || iy < 0 || iz < 0 || ix + 1 >= (s32)nx || iy + 1 >= (s32)ny || iz + 1 >= (s32)nz) {
        return _outside;
    }

    const f32 c[2][2][2] = {
        { { _data[index((u32)ix, (u32)iy, (u32)iz)],
            _data[index((u32)ix, (u32)iy, (u32)iz + 1)] },
          { _data[index((u32)ix, (u32)iy + 1, (u32)iz)],
            _data[index((u32)ix, (u32)iy + 1, (u32)iz + 1)] } },
        { { _data[index((u32)ix + 1, (u32)iy, (u32)iz)],
            _data[index((u32)ix + 1, (u32)iy, (u32)iz + 1)] },
          { _data[index((u32)ix + 1, (u32)iy + 1, (u32)iz)],
            _data[index((u32)ix + 1, (u32)iy + 1, (u32)iz + 1)] } },
    };
    const f32 c00 = lerp_m(c[0][0][0], c[1][0][0], tx);
    const f32 c01 = lerp_m(c[0][0][1], c[1][0][1], tx);
    const f32 c10 = lerp_m(c[0][1][0], c[1][1][0], tx);
    const f32 c11 = lerp_m(c[0][1][1], c[1][1][1], tx);
    const f32 c0 = lerp_m(c00, c10, ty);
    const f32 c1 = lerp_m(c01, c11, ty);
    return lerp_m(c0, c1, tz);
}

math::v3 volume_grid::sample_vector(math::v3 world) const {
    if (_kind != kind::vector3) return math::v3{ 0.f, 0.f, 0.f };
    // 标量路径的三通道版（同一插值权重）
    f32 gx, gy, gz;
    world_to_grid(world, gx, gy, gz);
    const f32 x0 = std::floor(gx), y0 = std::floor(gy), z0 = std::floor(gz);
    const s32 ix = (s32)x0, iy = (s32)y0, iz = (s32)z0;
    const u32 nx = _dims[0], ny = _dims[1], nz = _dims[2];
    if (ix < 0 || iy < 0 || iz < 0 || ix + 1 >= (s32)nx || iy + 1 >= (s32)ny || iz + 1 >= (s32)nz) {
        return math::v3{ 0.f, 0.f, 0.f };
    }

    const f32 tx = gx - x0, ty = gy - y0, tz = gz - z0;
    math::v3 out{ 0.f, 0.f, 0.f };
    const u32 stride_z = _dims[0], stride_y = _dims[0] * _dims[1];
    f32 w[2][2][2];
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c)
                w[a][b][c] = (a ? tx : 1.f - tx) * (b ? ty : 1.f - ty) * (c ? tz : 1.f - tz);

    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c) {
                const size_t base = ((size_t)(iz + c) * stride_y + (size_t)(iy + b) * stride_z + (size_t)(ix + a)) * 3;
                const f32 wt = w[a][b][c];
                out.x += _data[base] * wt;
                out.y += _data[base + 1] * wt;
                out.z += _data[base + 2] * wt;
            }
    return out;
}

math::v3 volume_grid::sample_gradient(math::v3 world) const {
    const f32 e = _cell;
    const f32 px = sample_scalar(world + math::v3{ e, 0.f, 0.f });
    const f32 mx = sample_scalar(world - math::v3{ e, 0.f, 0.f });
    const f32 py = sample_scalar(world + math::v3{ 0.f, e, 0.f });
    const f32 my = sample_scalar(world - math::v3{ 0.f, e, 0.f });
    const f32 pz = sample_scalar(world + math::v3{ 0.f, 0.f, e });
    const f32 mz = sample_scalar(world - math::v3{ 0.f, 0.f, e });
    return math::v3{ (px - mx) / (2.f * e), (py - my) / (2.f * e), (pz - mz) / (2.f * e) };
}

void volume_grid::set_scalar(u32 ix, u32 iy, u32 iz, f32 v) {
    if (ix >= _dims[0] || iy >= _dims[1] || iz >= _dims[2]) return;
    _data[index(ix, iy, iz)] = v;
}

void volume_grid::set_vector(u32 ix, u32 iy, u32 iz, math::v3 v) {
    if (ix >= _dims[0] || iy >= _dims[1] || iz >= _dims[2]) return;
    const size_t base = index(ix, iy, iz) * 3;
    _data[base] = v.x;
    _data[base + 1] = v.y;
    _data[base + 2] = v.z;
}

// ---- .pgrid ------------------------------------------------------------------
// 布局（小端）："PGRD" | u32 version(1) | u32 kind | u32 nx,ny,nz | f32 ox,oy,oz
//             | f32 cell | f32 outside | f32 data[...]

bool volume_grid::write_pgrid(const char* path, std::string& err) const {
    if (!is_initialized()) {
        err = "grid not initialized";
        return false;
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        err = std::string("cannot write '") + path + "'";
        return false;
    }
    u32 header[5] = { 0x44524750u,   // "PGRD" 小端
                      1u, (u32)_kind, _dims[0], _dims[1] };
    std::fwrite(header, sizeof(u32), 5, f);
    u32 nz = _dims[2];
    std::fwrite(&nz, sizeof(u32), 1, f);
    f32 origin[3] = { _origin.x, _origin.y, _origin.z };
    std::fwrite(origin, sizeof(f32), 3, f);
    std::fwrite(&_cell, sizeof(f32), 1, f);
    std::fwrite(&_outside, sizeof(f32), 1, f);
    std::fwrite(_data.data(), sizeof(f32), _data.size(), f);
    std::fclose(f);
    return true;
}

bool volume_grid::read_pgrid(const char* path, std::string& err) {
    shutdown();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        err = std::string("cannot open '") + path + "'";
        return false;
    }
    u32 header[5] = {};
    if (std::fread(header, sizeof(u32), 5, f) != 5 || header[0] != 0x44524750u) {
        std::fclose(f);
        err = "bad pgrid magic";
        return false;
    }
    if (header[1] != 1u) {
        std::fclose(f);
        err = "unsupported pgrid version";
        return false;
    }
    const u32 kind = header[2];
    const u32 nx = header[3];
    const u32 ny = header[4];
    u32 nz = 0;
    if (std::fread(&nz, sizeof(u32), 1, f) != 1) {
        std::fclose(f);
        err = "truncated pgrid header";
        return false;
    }
    f32 origin[3], cell, outside;
    if (std::fread(origin, sizeof(f32), 3, f) != 3 || std::fread(&cell, sizeof(f32), 1, f) != 1 ||
        std::fread(&outside, sizeof(f32), 1, f) != 1) {
        std::fclose(f);
        err = "truncated pgrid header";
        return false;
    }

    if (kind == (u32)kind::scalar) {
        init_scalar(nx, ny, nz, math::v3{ origin[0], origin[1], origin[2] }, cell, 0.f);
    } else {
        init_vector(nx, ny, nz, math::v3{ origin[0], origin[1], origin[2] }, cell,
                    math::v3{ 0.f, 0.f, 0.f });
    }
    _outside = outside;
    if (_data.size() && std::fread(_data.data(), sizeof(f32), _data.size(), f) != _data.size()) {
        shutdown();
        err = "truncated pgrid data";
        return false;
    }
    std::fclose(f);
    return true;
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
