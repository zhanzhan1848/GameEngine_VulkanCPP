#pragma once

#include "Engine/Common/CommonHeaders.h"
#include <cmath>
#include <algorithm>

namespace primal::graphics::pcg {

// Standard 2D Simplex noise implementation
// Output range: approximately [-1, 1]

inline f32 Simplex2D(f32 x, f32 y, u32 seed = 0) {
    // Skewing constants for 2D simplex
    constexpr f32 F2 = 0.3660254037844386f; // (sqrt(3) - 1) / 2
    constexpr f32 G2 = 0.21132486540518713f; // (3 - sqrt(3)) / 6

    // Build permutation table from seed
    static thread_local u32 perm[512];
    static thread_local u32 seed_cached = u32_invalid_id;
    if (seed_cached != seed) {
        seed_cached = seed;
        u32 p[256];
        for (u32 i = 0; i < 256; ++i) p[i] = i;
        // Fisher-Yates shuffle with LCG
        u32 s = seed;
        for (u32 i = 255; i > 0; --i) {
            s = s * 1103515245u + 12345u;
            u32 j = (s >> 16) % (i + 1);
            std::swap(p[i], p[j]);
        }
        for (u32 i = 0; i < 256; ++i) {
            perm[i] = p[i];
            perm[i + 256] = p[i];
        }
    }

    // 2D gradient vectors
    constexpr f32 grad2[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1}
    };

    auto fast_floor = [](f32 v) -> s32 {
        s32 i = static_cast<s32>(v);
        return v < i ? i - 1 : i;
    };

    // Skew input space
    f32 s = (x + y) * F2;
    s32 i = fast_floor(x + s);
    s32 j = fast_floor(y + s);

    f32 t = (i + j) * G2;
    f32 X0 = i - t;
    f32 Y0 = j - t;
    f32 x0 = x - X0;
    f32 y0 = y - Y0;

    // Determine simplex triangle
    s32 i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else { i1 = 0; j1 = 1; }

    f32 x1 = x0 - i1 + G2;
    f32 y1 = y0 - j1 + G2;
    f32 x2 = x0 - 1.0f + 2.0f * G2;
    f32 y2 = y0 - 1.0f + 2.0f * G2;

    s32 ii = i & 255;
    s32 jj = j & 255;

    auto dot2 = [](const f32 g[2], f32 x, f32 y) { return g[0] * x + g[1] * y; };

    f32 n0 = 0, n1 = 0, n2 = 0;

    f32 t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0) {
        t0 *= t0;
        const f32* g = grad2[perm[ii + perm[jj]] & 7];
        n0 = t0 * t0 * dot2(g, x0, y0);
    }

    f32 t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0) {
        t1 *= t1;
        const f32* g = grad2[perm[ii + i1 + perm[jj + j1]] & 7];
        n1 = t1 * t1 * dot2(g, x1, y1);
    }

    f32 t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0) {
        t2 *= t2;
        const f32* g = grad2[perm[ii + 1 + perm[jj + 1]] & 7];
        n2 = t2 * t2 * dot2(g, x2, y2);
    }

    // Scale to [-1, 1]
    return 70.0f * (n0 + n1 + n2);
}

// Fractal Brownian Motion — layered Simplex noise
inline f32 FBM2D(f32 x, f32 y, u32 octaves = 6, f32 lacunarity = 2.0f,
                 f32 persistence = 0.5f, u32 seed = 0) {
    f32 value = 0.0f;
    f32 amplitude = 1.0f;
    f32 frequency = 1.0f;
    f32 max_amp = 0.0f;

    for (u32 i = 0; i < octaves; ++i) {
        value += amplitude * Simplex2D(x * frequency, y * frequency, seed + i);
        max_amp += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / max_amp;
}

} // namespace primal::graphics::pcg
