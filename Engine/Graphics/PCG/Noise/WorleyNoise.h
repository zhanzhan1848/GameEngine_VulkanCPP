#pragma once

#include "Engine/Common/CommonHeaders.h"
#include <cmath>
#include <algorithm>

namespace primal::graphics::pcg {

// 2D Worley (cellular) noise — returns distance to the closest feature point (F1).
// Output range: [0, ~1.1] (unnormalized, distance in grid space)
//
// Uses a seeded permutation table matching SimplexNoise convention.
inline f32 WorleyF1_2D(f32 x, f32 y, u32 seed = 0) {
    static thread_local u32 perm[512];
    static thread_local u32 seed_cached = u32_invalid_id;
    if (seed_cached != seed) {
        seed_cached = seed;
        u32 p[256];
        for (u32 i = 0; i < 256; ++i) p[i] = i;
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

    auto fast_floor = [](f32 v) -> s32 {
        s32 i = static_cast<s32>(v);
        return v < i ? i - 1 : i;
    };

    s32 ix = fast_floor(x);
    s32 iy = fast_floor(y);

    f32 min_dist = 1e10f;

    // Check 3x3 neighborhood
    for (s32 dy = -1; dy <= 1; ++dy) {
        for (s32 dx = -1; dx <= 1; ++dx) {
            s32 cx = ix + dx;
            s32 cy = iy + dy;

            // Hash to get feature point offset within cell
            u32 h = perm[(cx & 255) + perm[(cy & 255)]];
            f32 fx = (h & 0xFF) / 255.0f;
            h = perm[(h & 255) + 1];
            f32 fy = (h & 0xFF) / 255.0f;

            f32 px = cx + fx;
            f32 py = cy + fy;
            f32 dist = (x - px) * (x - px) + (y - py) * (y - py);
            min_dist = std::min(min_dist, dist);
        }
    }

    return std::sqrt(min_dist);
}

// F2 variant — distance to the second closest feature point.
// Useful for creating more cellular patterns (F2-F1 gives edge detection).
inline f32 WorleyF2_2D(f32 x, f32 y, u32 seed = 0) {
    static thread_local u32 perm2[512];
    static thread_local u32 seed2_cached = u32_invalid_id;
    if (seed2_cached != seed) {
        seed2_cached = seed;
        u32 p[256];
        for (u32 i = 0; i < 256; ++i) p[i] = i;
        u32 s = seed;
        for (u32 i = 255; i > 0; --i) {
            s = s * 1103515245u + 12345u;
            u32 j = (s >> 16) % (i + 1);
            std::swap(p[i], p[j]);
        }
        for (u32 i = 0; i < 256; ++i) {
            perm2[i] = p[i];
            perm2[i + 256] = p[i];
        }
    }

    auto fast_floor = [](f32 v) -> s32 {
        s32 i = static_cast<s32>(v);
        return v < i ? i - 1 : i;
    };

    s32 ix = fast_floor(x);
    s32 iy = fast_floor(y);

    f32 d1 = 1e10f, d2 = 1e10f;

    for (s32 dy = -1; dy <= 1; ++dy) {
        for (s32 dx = -1; dx <= 1; ++dx) {
            s32 cx = ix + dx;
            s32 cy = iy + dy;

            u32 h = perm2[(cx & 255) + perm2[(cy & 255)]];
            f32 fx = (h & 0xFF) / 255.0f;
            h = perm2[(h & 255) + 1];
            f32 fy = (h & 0xFF) / 255.0f;

            f32 px = cx + fx;
            f32 py = cy + fy;
            f32 dist = (x - px) * (x - px) + (y - py) * (y - py);

            if (dist < d1) {
                d2 = d1;
                d1 = dist;
            } else if (dist < d2) {
                d2 = dist;
            }
        }
    }

    return std::sqrt(d2);
}

// Ridged multifractal noise — absolute value of Simplex with inverted FBM.
// Output range: [0, 1]. Creates ridge-like patterns.
inline f32 RidgedFBM2D(f32 x, f32 y, u32 octaves = 6, f32 lacunarity = 2.0f,
                       f32 persistence = 0.5f, u32 seed = 0) {
    f32 value = 0.0f;
    f32 amplitude = 1.0f;
    f32 frequency = 1.0f;
    f32 weight = 1.0f;

    for (u32 i = 0; i < octaves; ++i) {
        f32 signal = Simplex2D(x * frequency, y * frequency, seed + i);
        signal = 1.0f - std::abs(signal); // Create ridges
        signal *= signal;                  // Sharpen ridges
        signal *= weight;
        weight = std::clamp(signal * 2.0f, 0.0f, 1.0f);

        value += amplitude * signal;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    // Normalize to approximately [0, 1]
    return std::clamp(value * 0.5f, 0.0f, 1.0f);
}

} // namespace primal::graphics::pcg
