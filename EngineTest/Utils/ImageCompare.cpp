/**
 * @file ImageCompare.cpp
 * @brief STB_IMAGE_WRITE_IMPLEMENTATION + STB_IMAGE_IMPLEMENTATION live here
 *        (single TU). Wang-Bovik SSIM on RGBA8 luminance.
 */

#include "ImageCompare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"  // third_party/stb

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"        // third_party/stb

namespace EngineTest {

bool SavePNG(const char* path, const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) {
    // stbi_write_png uses stride = w*channels; row-major top-to-bottom.
    int ok = stbi_write_png(path, static_cast<int>(w), static_cast<int>(h),
                            4, rgba, static_cast<int>(w) * 4);
    return ok != 0;
}

bool LoadPNG(const char* path, std::vector<std::uint8_t>& out,
             std::uint32_t& w, std::uint32_t& h) {
    int x = 0, y = 0, channels = 0;
    unsigned char* data = stbi_load(path, &x, &y, &channels, 4 /* force RGBA */);
    if (!data) return false;
    w = static_cast<std::uint32_t>(x);
    h = static_cast<std::uint32_t>(y);
    out.assign(data, data + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(data);
    return true;
}

// IEEE 754 binary16 → binary32 (little-endian agnostic: we read raw u16).
static float half_to_float(std::uint16_t h) {
    std::uint32_t sign = (h >> 15) & 1u;
    std::uint32_t exp  = (h >> 10) & 0x1fu;
    std::uint32_t mant = h & 0x3ffu;
    float f;
    if (exp == 0) {
        if (mant == 0) {
            f = 0.0f;
        } else {
            // Subnormal: normalize.
            exp = 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3ffu;
            f = std::ldexp(static_cast<float>(mant), static_cast<int>(exp) - 15 - 10);
        }
    } else if (exp == 31) {
        f = (mant == 0) ? std::numeric_limits<float>::infinity()
                        : std::numeric_limits<float>::quiet_NaN();
    } else {
        f = std::ldexp(static_cast<float>(mant | 0x400u),
                       static_cast<int>(exp) - 15 - 10);
    }
    return sign ? -f : f;
}

void RGBA16FToRGBA8(const std::uint8_t* src, std::uint8_t* dst,
                    std::uint32_t w, std::uint32_t h) {
    const std::size_t npix = std::size_t(w) * std::size_t(h);
    for (std::size_t i = 0; i < npix; ++i) {
        for (int c = 0; c < 3; ++c) {  // RGB only; A passes through separately.
            std::uint16_t hf = std::uint16_t(src[i * 8 + c * 2])
                             | (std::uint16_t(src[i * 8 + c * 2 + 1]) << 8);
            float v = half_to_float(hf);
            if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
            // Reinhard tonemap then sRGB curve.
            float mapped = v / (1.0f + v);
            float encoded = (mapped <= 0.0031308f)
                            ? 12.92f * mapped
                            : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
            int byte = int(encoded * 255.0f + 0.5f);
            if (byte < 0) byte = 0;
            if (byte > 255) byte = 255;
            dst[i * 4 + c] = std::uint8_t(byte);
        }
        // Alpha: 16F → just clamp to [0, 1].
        std::uint16_t hf = std::uint16_t(src[i * 8 + 6])
                         | (std::uint16_t(src[i * 8 + 7]) << 8);
        float a = half_to_float(hf);
        if (!std::isfinite(a) || a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        dst[i * 4 + 3] = std::uint8_t(a * 255.0f + 0.5f);
    }
}

void FlipYInPlace(std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0) return;
    const std::size_t row_bytes = std::size_t(w) * 4;
    std::vector<std::uint8_t> tmp(row_bytes);
    for (std::uint32_t y = 0; y < h / 2; ++y) {
        std::uint8_t* top = rgba + std::size_t(y) * row_bytes;
        std::uint8_t* bot = rgba + std::size_t(h - 1 - y) * row_bytes;
        std::memcpy(tmp.data(), top, row_bytes);
        std::memcpy(top, bot, row_bytes);
        std::memcpy(bot, tmp.data(), row_bytes);
    }
}

void Depth32ToRGBA8(const std::uint8_t* src, std::uint8_t* dst,
                    std::uint32_t w, std::uint32_t h) {
    const std::size_t npix = std::size_t(w) * h;
    for (std::size_t i = 0; i < npix; ++i) {
        // Read little-endian f32.
        std::uint32_t u = std::uint32_t(src[i * 4 + 0])
                        | (std::uint32_t(src[i * 4 + 1]) << 8)
                        | (std::uint32_t(src[i * 4 + 2]) << 16)
                        | (std::uint32_t(src[i * 4 + 3]) << 24);
        float f;
        std::memcpy(&f, &u, 4);
        if (!std::isfinite(f) || f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        // Linear [0,1] → [0,255] grayscale (no sRGB — depth buffers are linear).
        std::uint8_t byte = static_cast<std::uint8_t>(f * 255.0f + 0.5f);
        dst[i * 4 + 0] = byte;
        dst[i * 4 + 1] = byte;
        dst[i * 4 + 2] = byte;
        dst[i * 4 + 3] = 255;
    }
}

// Rec.709 luminance from an RGBA8 pixel.
static inline float luminance(const std::uint8_t* p) {
    float r = p[0] / 255.0f;
    float g = p[1] / 255.0f;
    float b = p[2] / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float ComputeSSIM(const std::uint8_t* a, const std::uint8_t* b,
                  std::uint32_t w, std::uint32_t h) {
    if (w < 8 || h < 8) {
        // For tiny images fall back to mean absolute luminance similarity in [0, 1].
        if (w == 0 || h == 0) return 0.0f;
        double acc = 0.0;
        std::size_t n = std::size_t(w) * h;
        for (std::size_t i = 0; i < n; ++i) {
            float la = luminance(a + i * 4);
            float lb = luminance(b + i * 4);
            acc += 1.0 - std::min(1.0, std::fabs(double(la) - double(lb)));
        }
        return float(acc / double(n));
    }

    // 8×8 sliding window, Gaussian-weighted (sigma=1.5), per Wang et al. 2004.
    // Constants: K1=0.01, K2=0.03, L=255 (8-bit). C1=(K1*L)^2, C2=(K2*L)^2.
    constexpr float K1 = 0.01f, K2 = 0.03f, L = 255.0f;
    constexpr float C1 = (K1 * L) * (K1 * L);
    constexpr float C2 = (K2 * L) * (K2 * L);

    // Pre-compute 8x8 Gaussian weights, normalized to sum=1.
    float gw[8][8];
    {
        constexpr float sigma = 1.5f;
        constexpr float two_sigma_sq = 2.0f * sigma * sigma;
        float sum = 0.0f;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                float dx = x - 3.5f;
                float dy = y - 3.5f;
                gw[y][x] = std::exp(-(dx * dx + dy * dy) / two_sigma_sq);
                sum += gw[y][x];
            }
        }
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) gw[y][x] /= sum;
    }

    double ssim_sum = 0.0;
    std::size_t num_windows = 0;

    for (std::uint32_t wy = 0; wy + 8 <= h; ++wy) {
        for (std::uint32_t wx = 0; wx + 8 <= w; ++wx) {
            double mu_a = 0.0, mu_b = 0.0;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    std::size_t idx = (std::size_t(wy + y) * w + (wx + x)) * 4;
                    float la = luminance(a + idx) * 255.0f;
                    float lb = luminance(b + idx) * 255.0f;
                    mu_a += la * gw[y][x];
                    mu_b += lb * gw[y][x];
                }
            }

            double var_a = 0.0, var_b = 0.0, cov = 0.0;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    std::size_t idx = (std::size_t(wy + y) * w + (wx + x)) * 4;
                    float la = luminance(a + idx) * 255.0f;
                    float lb = luminance(b + idx) * 255.0f;
                    float da = float(la - mu_a);
                    float db = float(lb - mu_b);
                    var_a += da * da * gw[y][x];
                    var_b += db * db * gw[y][x];
                    cov    += da * db * gw[y][x];
                }
            }

            double num = (2.0 * mu_a * mu_b + C1) * (2.0 * cov + C2);
            double den = (mu_a * mu_a + mu_b * mu_b + C1) * (var_a + var_b + C2);
            ssim_sum += num / den;
            ++num_windows;
        }
    }

    return static_cast<float>(ssim_sum / double(num_windows));
}

} // namespace EngineTest
