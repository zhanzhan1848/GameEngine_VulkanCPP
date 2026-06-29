// PNG writer for `--headless-render` CLI mode: dumps a single RGBA16F
// frame to disk without ever invoking NSApp or talking to WindowServer.
//
// Uses libz (already linked) for IDAT compression and a hand-rolled
// IEEE 754 binary16 → float decoder for the half-float → RGBA8 path.

#include "Common.h"
#include "EngineAPIInternal.h"
#include "../Graphics/RenderTexture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include <zlib.h>

using namespace primal;

namespace
{
    constexpr uint8_t kPngSignature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    uint32_t crc32_compute(const uint8_t* data, size_t len) {
        // Use libz's crc32 if available — much faster than a hand-rolled one.
        return crc32(0L, data, static_cast<uInt>(len));
    }

    void append_be32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
        out.push_back(static_cast<uint8_t>( v        & 0xFF));
    }

    void append_chunk(std::vector<uint8_t>& out,
                      const char* type,
                      const std::vector<uint8_t>& data) {
        const uint32_t length = static_cast<uint32_t>(data.size());
        append_be32(out, length);

        const size_t type_offset = out.size();
        out.insert(out.end(), type, type + 4);  // 4-byte chunk type
        out.insert(out.end(), data.begin(), data.end());

        const uint8_t* crc_input = out.data() + type_offset;
        const uint32_t crc = crc32_compute(crc_input, out.size() - type_offset);
        append_be32(out, crc);
    }

    // IEEE 754 binary16 → float. Bit layout: sign(1) | exp(5) | mantissa(10).
    float half_to_float(uint16_t h) {
        uint32_t sign = (h >> 15) & 0x1;
        uint32_t exp  = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;

        uint32_t out_bits;
        if (exp == 0) {
            if (mant == 0) {
                out_bits = sign << 31;  // ±0
            } else {
                // Subnormal: convert to normalized
                uint32_t e = 1;
                while ((mant & 0x400) == 0) { mant <<= 1; e++; }
                mant &= 0x3FF;
                out_bits = (sign << 31) | ((127 - 15 - e + 1) << 23) | (mant << 13);
            }
        } else if (exp == 31) {
            out_bits = (sign << 31) | (0x1FF << 23) | (mant << 13);  // Inf/NaN
        } else {
            out_bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
        }

        float result;
        std::memcpy(&result, &out_bits, sizeof(result));
        return result;
    }

    uint8_t clamp_to_u8(float v) {
        if (std::isnan(v)) return 0;
        if (v <= 0.f) return 0;
        if (v >= 1.f) return 255;
        return static_cast<uint8_t>(v * 255.f + 0.5f);
    }

    // Convert one RGBA16F scanline to filtered RGBA8 scanline (filter=0).
    void convert_scanline(const uint8_t* src_rgba16f,
                          std::vector<uint8_t>& dst,
                          uint32_t width) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(src_rgba16f);
        dst.resize(static_cast<size_t>(width) * 4 + 1);
        dst[0] = 0;  // filter type = None
        for (uint32_t x = 0; x < width; ++x) {
            const float r = half_to_float(src[x * 4 + 0]);
            const float g = half_to_float(src[x * 4 + 1]);
            const float b = half_to_float(src[x * 4 + 2]);
            const float a = half_to_float(src[x * 4 + 3]);
            dst[1 + x * 4 + 0] = clamp_to_u8(r);
            dst[1 + x * 4 + 1] = clamp_to_u8(g);
            dst[1 + x * 4 + 2] = clamp_to_u8(b);
            dst[1 + x * 4 + 3] = clamp_to_u8(a);
        }
    }

    // zlib-compress the entire IDAT payload (deflate, default level).
    bool deflate_idat(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
        z_stream zs{};
        if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return false;

        // Worst case bound: 0.1% overhead + 12 bytes per block.
        const uLong bound = deflateBound(&zs, static_cast<uLong>(raw.size()));
        out.resize(bound);

        zs.next_in = const_cast<Bytef*>(raw.data());
        zs.avail_in = static_cast<uInt>(raw.size());

        // Drain loop.
        size_t produced = 0;
        int ret = Z_OK;
        while (ret != Z_STREAM_END) {
            if (produced + 4096 > out.size()) out.resize(produced + 4096);
            zs.next_out = out.data() + produced;
            zs.avail_out = static_cast<uInt>(out.size() - produced);
            ret = deflate(&zs, Z_FINISH);
            if (ret == Z_STREAM_ERROR || ret < 0) { deflateEnd(&zs); return false; }
            produced = zs.total_out;
        }
        deflateEnd(&zs);
        out.resize(produced);
        return true;
    }
}  // namespace

extern "C" {

EDITOR_INTERFACE u32 DebugReadRenderTarget(u64, void*, u64);

// Write an RGBA16F render target to disk as PNG (RGBA8).
// Half-floats are clamped 0..1 → 0..255. Inf = 255, NaN = 0.
// Returns 1 on success, 0 on any failure (alloc, file I/O, deflate).
EDITOR_INTERFACE int SaveRenderTargetAsPng(u64 target_handle,
                                          const char* path) {
    auto* entry = primal::engine_dll::GetRenderTarget(target_handle);
    if (!entry || !entry->texture) {
        std::fprintf(stderr, "[SavePNG] invalid target handle %llu\n",
                     static_cast<unsigned long long>(target_handle));
        return 0;
    }
    const auto& desc = entry->texture->GetDesc();
    const uint32_t width = desc.size.x;
    const uint32_t height = desc.size.y;
    if (width == 0 || height == 0) {
        std::fprintf(stderr, "[SavePNG] invalid target dimensions %ux%u\n",
                     width, height);
        return 0;
    }

    // RGBA16F = 8 bytes per pixel (4 half-floats).
    const u64 pixel_bytes = static_cast<u64>(width) * height * 8;
    std::vector<uint8_t> rgba16f(pixel_bytes);
    if (DebugReadRenderTarget(target_handle, rgba16f.data(), pixel_bytes) != 1) {
        std::fprintf(stderr, "[SavePNG] DebugReadRenderTarget failed\n");
        return 0;
    }

    // Build raw IDAT payload: filter byte + RGBA8 per scanline.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width) * height * 4 + height);
    std::vector<uint8_t> scanline;
    for (u32 y = 0; y < height; ++y) {
        convert_scanline(rgba16f.data() + y * width * 8, scanline, width);
        raw.insert(raw.end(), scanline.begin(), scanline.end());
    }

    std::vector<uint8_t> idat;
    if (!deflate_idat(raw, idat)) {
        std::fprintf(stderr, "[SavePNG] deflate failed\n");
        return 0;
    }

    std::vector<uint8_t> png;
    png.insert(png.end(), kPngSignature, kPngSignature + 8);

    // IHDR
    std::vector<uint8_t> ihdr(13);
    ihdr[0] = (width  >> 24) & 0xFF; ihdr[1] = (width  >> 16) & 0xFF;
    ihdr[2] = (width  >>  8) & 0xFF; ihdr[3] =  width         & 0xFF;
    ihdr[4] = (height >> 24) & 0xFF; ihdr[5] = (height >> 16) & 0xFF;
    ihdr[6] = (height >>  8) & 0xFF; ihdr[7] = height        & 0xFF;
    ihdr[8] = 8;    // bit depth per channel
    ihdr[9] = 6;    // color type: RGBA
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter
    ihdr[12] = 0;   // interlace
    append_chunk(png, "IHDR", ihdr);

    append_chunk(png, "IDAT", idat);
    append_chunk(png, "IEND", {});

    const char* out_path = path;
    if (!out_path) {
        out_path = "headless_render.png";
    }
    FILE* fp = std::fopen(out_path, "wb");
    if (!fp) {
        std::fprintf(stderr, "[SavePNG] cannot open '%s' for writing\n", out_path);
        return 0;
    }
    const size_t wrote = std::fwrite(png.data(), 1, png.size(), fp);
    std::fclose(fp);
    if (wrote != png.size()) {
        std::fprintf(stderr, "[SavePNG] short write to '%s'\n", out_path);
        return 0;
    }
    std::fprintf(stderr, "[SavePNG] wrote %zux%zu RGBA8 (%zu bytes) to '%s'\n",
                 static_cast<size_t>(width), static_cast<size_t>(height),
                 png.size(), out_path);
    return 1;
}

}  // extern "C"
