/**
 * @file VTBaker.cpp
 * @brief VT M1 — Virtual Texture 离线烘焙实现
 * @details 管线:RGBA 源 → box-filter mip 链(至 tile 尺寸)→ 逐页提取
 *          (tile+border,clamp 扩展)→ BCn(内置编码器)+ ASTC(astc-encoder)
 *          双段编码(可选 zlib)→ VTEX 容器组装(页索引按 mip 聚类)。
 *
 * BC 编码器为"够用"级(luminance 端点拟合 + 最近索引),Sponza 级内容
 * PSNR 30dB 上下;Windows 生产路径后续换 DirectXTex 高质量编码器,
 * 容器与运行时不受影响。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-21
 */

#include "VTBaker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>

#include <zlib.h>

#include "astcenc.h"

namespace primal::content::vt {

namespace {

// ============================================================================
// mip 链(RGBA8 box filter;最低到 tile 边长 — 更粗 mip 属于页表回退范围)
// ============================================================================
struct mip_level {
    std::vector<uint8_t> rgba;
    uint32_t side_x = 0;
    uint32_t side_y = 0;
};

std::vector<mip_level> generate_mips(const uint8_t* rgba, uint32_t w, uint32_t h,
                                     uint32_t tile) {
    std::vector<mip_level> mips;
    mip_level m0;
    m0.rgba.assign(rgba, rgba + size_t(w) * h * 4);
    m0.side_x = w; m0.side_y = h;
    mips.push_back(std::move(m0));
    while (mips.back().side_x > tile || mips.back().side_y > tile) {
        const mip_level& prev = mips.back();
        mip_level next;
        next.side_x = std::max<uint32_t>(1, prev.side_x / 2);
        next.side_y = std::max<uint32_t>(1, prev.side_y / 2);
        next.rgba.resize(size_t(next.side_x) * next.side_y * 4);
        for (uint32_t y = 0; y < next.side_y; ++y) {
            uint32_t sy0 = std::min(y * 2, prev.side_y - 1);
            uint32_t sy1 = std::min(y * 2 + 1, prev.side_y - 1);
            for (uint32_t x = 0; x < next.side_x; ++x) {
                uint32_t sx0 = std::min(x * 2, prev.side_x - 1);
                uint32_t sx1 = std::min(x * 2 + 1, prev.side_x - 1);
                for (uint32_t c = 0; c < 4; ++c) {
                    uint32_t sum = prev.rgba[(size_t(sy0) * prev.side_x + sx0) * 4 + c]
                                 + prev.rgba[(size_t(sy0) * prev.side_x + sx1) * 4 + c]
                                 + prev.rgba[(size_t(sy1) * prev.side_x + sx0) * 4 + c]
                                 + prev.rgba[(size_t(sy1) * prev.side_x + sx1) * 4 + c];
                    next.rgba[(size_t(y) * next.side_x + x) * 4 + c] = uint8_t((sum + 2) / 4);
                }
            }
        }
        mips.push_back(std::move(next));
    }
    return mips;
}

// 提取 (px,py) 页:tile + border clamp 扩展(M0 同款;邻页边缘扩展属 M3)
std::vector<uint8_t> extract_tile(const mip_level& mip, uint32_t px, uint32_t py,
                                  uint32_t tile, uint32_t border) {
    const uint32_t phys = tile + 2 * border;
    std::vector<uint8_t> out(size_t(phys) * phys * 4);
    auto sample_clamped = [&](int sx, int sy) -> const uint8_t* {
        sx = std::clamp(sx, 0, int(mip.side_x) - 1);
        sy = std::clamp(sy, 0, int(mip.side_y) - 1);
        return &mip.rgba[(size_t(sy) * mip.side_x + sx) * 4];
    };
    for (uint32_t y = 0; y < phys; ++y)
        for (uint32_t x = 0; x < phys; ++x) {
            int sx = int(px * tile) + int(x) - int(border);
            int sy = int(py * tile) + int(y) - int(border);
            std::memcpy(&out[(size_t(y) * phys + x) * 4],
                        sample_clamped(sx, sy), 4);
        }
    return out;
}

// ============================================================================
// BC 编码器(4x4 block;luminance 拟合)
// ============================================================================
uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((uint32_t(r) >> 3) << 11 | (uint32_t(g) >> 2) << 5 | (uint32_t(b) >> 3));
}
void unpack565(uint16_t p, uint8_t out[3]) {
    out[0] = uint8_t(((p >> 11) & 0x1F) * 255 / 31);
    out[1] = uint8_t(((p >> 5) & 0x3F) * 255 / 63);
    out[2] = uint8_t((p & 0x1F) * 255 / 31);
}

// 16 texel RGB(A 忽略)→ 8 字节 BC1
void encode_bc1_block(const uint8_t* rgba /*64B*/, uint8_t out[8]) {
    // luminance 端点拟合:min/max 投影
    float lum[16];
    float lmin = 1e9f, lmax = -1e9f;
    int imin = 0, imax = 0;
    for (int i = 0; i < 16; ++i) {
        lum[i] = 0.299f * rgba[i * 4 + 0] + 0.587f * rgba[i * 4 + 1] + 0.114f * rgba[i * 4 + 2];
        if (lum[i] < lmin) { lmin = lum[i]; imin = i; }
        if (lum[i] > lmax) { lmax = lum[i]; imax = i; }
    }
    uint16_t c0 = pack565(rgba[imax * 4], rgba[imax * 4 + 1], rgba[imax * 4 + 2]);
    uint16_t c1 = pack565(rgba[imin * 4], rgba[imin * 4 + 1], rgba[imin * 4 + 2]);
    // 4-color 模式要求 c0 > c1(数值比较);相等时扰动 c0
    if (c0 <= c1) {
        if (c0 == c1) c0 = uint16_t(c0 == 0x1F ? 0x1F : c0 | 1);
        else std::swap(c0, c1);
    }
    // 4 色调色板
    uint8_t p0[3], p1[3], pal[4][3];
    unpack565(c0, p0); unpack565(c1, p1);
    for (int c = 0; c < 3; ++c) {
        pal[0][c] = p0[c];
        pal[1][c] = p1[c];
        pal[2][c] = uint8_t((uint32_t(p0[c]) * 2 + p1[c]) / 3);
        pal[3][c] = uint8_t((uint32_t(p0[c]) + p1[c] * 2) / 3);
    }
    float pl[4];
    for (int i = 0; i < 4; ++i)
        pl[i] = 0.299f * pal[i][0] + 0.587f * pal[i][1] + 0.114f * pal[i][2];

    out[0] = uint8_t(c0 & 0xFF); out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1 & 0xFF); out[3] = uint8_t(c1 >> 8);
    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0; float bd = 1e9f;
        for (int p = 0; p < 4; ++p) {
            float d = std::fabs(lum[i] - pl[p]);
            if (d < bd) { bd = d; best = p; }
        }
        indices |= uint32_t(best) << (2 * i);
    }
    for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
}

// 16 texel 单通道 → 8 字节 BC4(8-value 调色板,a0=max ≥ a1=min)
void encode_bc4_block(const uint8_t* chan /*16B*/, uint8_t out[8]) {
    uint8_t a0 = *std::max_element(chan, chan + 16);
    uint8_t a1 = *std::min_element(chan, chan + 16);
    if (a0 < a1) std::swap(a0, a1);
    else if (a0 == a1) { a0 = uint8_t(a1 == 255 ? 254 : a1 + 1); }
    uint8_t pal[8] = {a0, a1,
        uint8_t((uint32_t(a0) * 6 + a1) / 7), uint8_t((uint32_t(a0) * 5 + a1 * 2) / 7),
        uint8_t((uint32_t(a0) * 4 + a1 * 3) / 7), uint8_t((uint32_t(a0) * 3 + a1 * 4) / 7),
        uint8_t((uint32_t(a0) * 2 + a1 * 5) / 7), uint8_t((uint32_t(a0) + a1 * 6) / 7)};
    out[0] = a0; out[1] = a1;
    uint32_t indices = 0;  // 16 × 3bit,LSB 先
    for (int i = 0; i < 16; ++i) {
        int best = 0; int bd = 255;
        for (int p = 0; p < 8; ++p) {
            int d = std::abs(int(chan[i]) - int(pal[p]));
            if (d < bd) { bd = d; best = p; }
        }
        indices |= uint32_t(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t((indices >> (8 * i)) & 0xFF);
}

// 整 tile(tile+2*border,4 对齐)编码为 BCn 字节流
std::vector<uint8_t> encode_tile_bcn(const std::vector<uint8_t>& tile, uint32_t phys,
                                     channel_kind kind) {
    const uint32_t bx = phys / 4, by = phys / 4;
    const size_t bytesPerBlock = (kind == channel_kind::color) ? 8 : (kind == channel_kind::normal ? 16 : 8);
    std::vector<uint8_t> out(size_t(bx) * by * bytesPerBlock, 0);
    for (uint32_t by0 = 0; by0 < by; ++by0)
        for (uint32_t bx0 = 0; bx0 < bx; ++bx0) {
            uint8_t blockRGBA[64];
            uint8_t chanR[16], chanG[16];
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint8_t* s = &tile[(size_t(by0 * 4 + y) * phys + bx0 * 4 + x) * 4];
                    uint32_t ti = y * 4 + x;
                    std::memcpy(&blockRGBA[ti * 4], s, 4);
                    chanR[ti] = s[0];
                    chanG[ti] = s[1];
                }
            uint8_t* dst = &out[(size_t(by0) * bx + bx0) * bytesPerBlock];
            if (kind == channel_kind::color) {
                encode_bc1_block(blockRGBA, dst);
            } else if (kind == channel_kind::normal) {
                encode_bc4_block(chanR, dst);          // R 通道
                encode_bc4_block(chanG, dst + 8);      // G 通道
            } else {
                encode_bc4_block(chanR, dst);
            }
        }
    return out;
}

// ============================================================================
// ASTC(astc-encoder;上下文烘焙期间复用)
// ============================================================================
struct astc_encoder_ctx {
    astcenc_context* ctx = nullptr;
    astcenc_config cfg{};
    bool init(float quality) {
        float q = quality > 0.0f ? quality : 25.0f;  // 0 = 默认快速档
        astcenc_error err = astcenc_config_init(ASTCENC_PRF_LDR,
                                                4, 4, 1, q, 0, &cfg);
        if (err != ASTCENC_SUCCESS) return false;
        err = astcenc_context_alloc(&cfg, 1, &ctx);
        return err == ASTCENC_SUCCESS;
    }
    ~astc_encoder_ctx() {
        if (ctx) astcenc_context_free(ctx);
    }
    // 返回编码后的 tile 字节(4x4 block × (phys/4)² × 16B)
    std::vector<uint8_t> encode_tile(const std::vector<uint8_t>& tile, uint32_t phys) {
        astcenc_image img{};
        img.dim_x = phys;
        img.dim_y = phys;
        img.dim_z = 1;
        void* ptr = const_cast<uint8_t*>(tile.data());
        img.data = &ptr;
        img.data_type = ASTCENC_TYPE_U8;
        astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const size_t blocks = size_t(phys / 4) * (phys / 4);
        std::vector<uint8_t> out(blocks * 16, 0);
        astcenc_error err = astcenc_compress_image(ctx, &img, &swz,
                                                   out.data(), out.size(), 0);
        if (err != ASTCENC_SUCCESS) {
            std::cerr << "[VTBaker] astcenc_compress_image failed: "
                      << astcenc_get_error_string(err) << std::endl;
            return {};
        }
        astcenc_compress_reset(ctx);
        return out;
    }
};

// ============================================================================
// zlib
// ============================================================================
bool zlib_compress(const uint8_t* src, size_t size, std::vector<uint8_t>& out) {
    uLongf bound = compressBound(uLong(size));
    out.resize(bound);
    uLongf written = bound;
    if (compress2(out.data(), &written, src, uLong(size), Z_BEST_SPEED) != Z_OK)
        return false;
    out.resize(written);
    return true;
}

// root 平均色(RGBA8,不透明白)
uint32_t compute_avg_color(const uint8_t* rgba, size_t pixelCount) {
    uint64_t r = 0, g = 0, b = 0;
    for (size_t i = 0; i < pixelCount; ++i) {
        r += rgba[i * 4 + 0]; g += rgba[i * 4 + 1]; b += rgba[i * 4 + 2];
    }
    if (pixelCount == 0) return 0xFF000000;
    r /= pixelCount; g /= pixelCount; b /= pixelCount;
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

} // anonymous namespace

// ============================================================================
// bake 主流程
// ============================================================================
bool bake(const uint8_t* rgba, uint32_t width, uint32_t height,
          const bake_config& cfg, std::vector<uint8_t>& out_vtex,
          bake_stats* stats) {
    out_vtex.clear();
    if (!rgba || width == 0 || height == 0) return false;
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
        std::cerr << "[VTBaker] 尺寸须为 2 的幂(w=" << width << " h=" << height << ")" << std::endl;
        return false;
    }
    if (!cfg.validate()) {
        std::cerr << "[VTBaker] 配置非法(tile/border 对齐或格式开关)" << std::endl;
        return false;
    }

    // mip 链(至 tile 边长)
    std::vector<mip_level> mips = generate_mips(rgba, width, height, cfg.tile_size);
    const uint32_t mip_count = uint32_t(mips.size());

    // 页计数与索引(mip 升序 → 页 y → 页 x 聚类)
    uint32_t page_count = 0;
    for (uint32_t m = 0; m < mip_count; ++m) {
        uint32_t px = mips[m].side_x / cfg.tile_size;
        uint32_t py = mips[m].side_y / cfg.tile_size;
        page_count += std::max<uint32_t>(1, px) * std::max<uint32_t>(1, py);
    }
    // 最低 mip 边长可能 < tile(非正方形)→ 单页,对齐到 tile 网格语义

    std::vector<uint8_t> bcn_section, astc_section;
    std::vector<vtex_page_index> index(page_count);
    uint32_t page = 0;
    uint64_t bcn_total = 0, astc_total = 0;

    astc_encoder_ctx astc;
    if (cfg.use_astc && !astc.init(cfg.astc_quality)) {
        std::cerr << "[VTBaker] ASTC 上下文初始化失败" << std::endl;
        return false;
    }

    for (uint32_t m = 0; m < mip_count; ++m) {
        const uint32_t pages_x = std::max<uint32_t>(1, mips[m].side_x / cfg.tile_size);
        const uint32_t pages_y = std::max<uint32_t>(1, mips[m].side_y / cfg.tile_size);
        for (uint32_t py = 0; py < pages_y; ++py)
            for (uint32_t px = 0; px < pages_x; ++px) {
                std::vector<uint8_t> tile = extract_tile(mips[m], px, py,
                                                          cfg.tile_size, cfg.border);
                vtex_page_index& entry = index[page];
                entry.v_page_x = px;
                entry.v_page_y = py;
                entry.mip = m;

                if (cfg.use_bcn) {
                    std::vector<uint8_t> enc = encode_tile_bcn(tile,
                                                               cfg.tile_size + 2 * cfg.border,
                                                               cfg.kind);
                    if (cfg.zlib_tiles) zlib_compress(enc.data(), enc.size(), enc);
                    entry.bcn_offset = uint32_t(bcn_section.size());
                    entry.bcn_size = uint32_t(enc.size());
                    bcn_section.insert(bcn_section.end(), enc.begin(), enc.end());
                    bcn_total += enc.size();
                }
                if (cfg.use_astc) {
                    std::vector<uint8_t> enc = astc.encode_tile(tile,
                                                                cfg.tile_size + 2 * cfg.border);
                    if (cfg.zlib_tiles && !enc.empty())
                        zlib_compress(enc.data(), enc.size(), enc);
                    entry.astc_offset = uint32_t(astc_section.size());
                    entry.astc_size = uint32_t(enc.size());
                    astc_section.insert(astc_section.end(), enc.begin(), enc.end());
                    astc_total += enc.size();
                }
                ++page;
            }
    }

    // 组装容器
    const uint32_t bcn_format = !cfg.use_bcn ? 0 :
        (cfg.kind == channel_kind::color ? bcn_format::BC1_UNorm :
         cfg.kind == channel_kind::normal ? bcn_format::BC5_UNorm : bcn_format::BC4_UNorm);

    vtex_header hdr{};
    std::memcpy(hdr.magic, "VTEX", 4);
    hdr.version = kVTEXVersion;
    hdr.width = width;
    hdr.height = height;
    hdr.tile_size = cfg.tile_size;
    hdr.border = cfg.border;
    hdr.mip_count = mip_count;
    hdr.flags = cfg.zlib_tiles ? kVTEXFlagZlibTiles : 0;
    hdr.bcn_format = bcn_format;
    hdr.astc_format = cfg.use_astc ? astc_format::ASTC_4x4_UNorm : 0;
    hdr.page_count = page_count;
    hdr.avg_color = compute_avg_color(rgba, size_t(width) * height);
    hdr.index_offset = sizeof(vtex_header);
    hdr.bcn_offset = uint32_t(sizeof(vtex_header) + index.size() * sizeof(vtex_page_index));
    hdr.bcn_size = uint32_t(bcn_section.size());
    hdr.astc_offset = hdr.bcn_offset + hdr.bcn_size;
    hdr.astc_size = uint32_t(astc_section.size());

    out_vtex.reserve(hdr.astc_offset + hdr.astc_size);
    const uint8_t* hdrBytes = reinterpret_cast<const uint8_t*>(&hdr);
    out_vtex.insert(out_vtex.end(), hdrBytes, hdrBytes + sizeof(hdr));
    const uint8_t* idxBytes = reinterpret_cast<const uint8_t*>(index.data());
    out_vtex.insert(out_vtex.end(), idxBytes,
                    idxBytes + index.size() * sizeof(vtex_page_index));
    out_vtex.insert(out_vtex.end(), bcn_section.begin(), bcn_section.end());
    out_vtex.insert(out_vtex.end(), astc_section.begin(), astc_section.end());

    if (stats) {
        stats->mip_count = mip_count;
        stats->page_count = page_count;
        stats->bcn_bytes = bcn_total;
        stats->astc_bytes = astc_total;
        stats->total_bytes = out_vtex.size();
        stats->avg_color = hdr.avg_color;
    }
    return true;
}

} // namespace primal::content::vt
