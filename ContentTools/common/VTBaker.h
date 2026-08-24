/**
 * @file VTBaker.h
 * @brief VT M1 — Virtual Texture 离线烘焙库 + VTEX 容器格式定义
 * @details 跨平台(无 FBX/DirectXTex 依赖):stb 加载源图(CLI 侧)、内置
 *          BC1/BC4/BC5 编码器、third_party/astc-encoder 做 ASTC、可选 zlib。
 *          Windows 生产路径后续可直接换 DirectXTex 高质量 BCn,容器不变。
 *
 * 本文件同时是 VTEX 容器结构的单一事实源:Engine/Content/VTEXAsset.h
 * 直接 include 本头文件,两侧永不漂移。
 *
 * 容器布局(设计文档 §4.1):
 *   [vtex_header][vtex_page_index × page_count][BCn 数据段][ASTC 数据段]
 *   页索引按 mip 升序聚类(低 mip 连续存放,流送友好);
 *   段内偏移相对各自段起始;每 tile 可选 zlib(整 tile 压缩,含 border)。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-21
 */

#pragma once

#include <cstdint>
#include <vector>

namespace primal::content::vt {

// ============================================================================
// VTEX 容器结构(与 RHI DataFormat 枚举值镜像;数值见 RHITypes.h,勿改动)
// ============================================================================
namespace bcn_format {
constexpr uint32_t BC1_UNorm = 62;
constexpr uint32_t BC4_UNorm = 68;
constexpr uint32_t BC5_UNorm = 70;
} // namespace bcn_format
namespace astc_format {
constexpr uint32_t ASTC_4x4_UNorm = 80;
} // namespace astc_format

constexpr uint32_t kVTEXVersion = 1;
constexpr uint32_t kVTEXFlagZlibTiles = 0x1;

#pragma pack(push, 1)
struct vtex_header {
    char     magic[4];       // 'VTEX'
    uint32_t version;        // kVTEXVersion
    uint32_t width;          // 虚拟分辨率(2^n,非正方形允许)
    uint32_t height;
    uint32_t tile_size;      // 默认 128
    uint32_t border;         // 默认 4
    uint32_t mip_count;      // log2(max(w,h)/tile)+1(最低 mip ≤ tile)
    uint32_t flags;          // bit0: kVTEXFlagZlibTiles
    uint32_t bcn_format;     // bcn_format::*(0 = 无 BCn 段)
    uint32_t astc_format;    // astc_format::*(0 = 无 ASTC 段)
    uint32_t page_count;     // 全部 mip 的虚拟页总数
    uint32_t avg_color;      // mip0 平均色 RGBA8(RRRR pack,root 兜底)
    uint32_t index_offset;   // 相对文件头 → vtex_page_index[]
    uint32_t bcn_offset;     // 相对文件头
    uint32_t bcn_size;
    uint32_t astc_offset;
    uint32_t astc_size;
    uint32_t reserved[4];    // 预留(M2: layerCount/layerFormats/udim)
};
static_assert(sizeof(vtex_header) == 4 + 20 * 4, "vtex_header 布局");

struct vtex_page_index {
    uint32_t v_page_x;       // 虚拟页坐标(mip 层级内)
    uint32_t v_page_y;
    uint32_t mip;
    uint32_t bcn_offset;     // 相对 BCn 段起始
    uint32_t bcn_size;       // 0 = 该段不存在
    uint32_t astc_offset;    // 相对 ASTC 段起始
    uint32_t astc_size;
};
static_assert(sizeof(vtex_page_index) == 28, "vtex_page_index 布局");
#pragma pack(pop)

// ============================================================================
// 烘焙 API
// ============================================================================

/// 纹理通道语义(决定压缩格式选择)
enum class channel_kind : uint32_t {
    color  = 0,   // BC1(RGB)+ ASTC 4x4
    normal = 1,   // BC5(RG)+ ASTC 4x4
    scalar = 2,   // BC4(R)+ ASTC 4x4
};

struct bake_config {
    uint32_t    tile_size   = 128;
    uint32_t    border      = 4;
    channel_kind kind       = channel_kind::color;
    bool        use_bcn     = true;
    bool        use_astc    = true;
    bool        zlib_tiles  = false;  // 每 tile zlib(整 tile+border)
    float       astc_quality = 0.0f;  // astcenc effort 0-100(0 = FAST 档)

    bool validate() const {
        return tile_size >= 16 && (tile_size & (tile_size - 1)) == 0 &&
               border >= 2 && border <= 16 &&
               ((tile_size + 2 * border) % 4) == 0 &&  // BC/ASTC 4x4 块对齐
               (use_bcn || use_astc);
    }
};

/// 烘焙统计(CLI/测试输出用)
struct bake_stats {
    uint32_t mip_count  = 0;
    uint32_t page_count = 0;
    uint64_t bcn_bytes  = 0;
    uint64_t astc_bytes = 0;
    uint64_t total_bytes = 0;
    uint32_t avg_color  = 0;
};

/**
 * @brief 烘焙 RGBA8 源图 → VTEX 容器字节流
 * @param rgba    行主序 RGBA8,尺寸 width × height
 * @param cfg     烘焙配置
 * @param out_vtex 输出容器
 * @param[out] stats 可选统计
 * @return 成功与否(尺寸/配置非法返回 false)
 */
bool bake(const uint8_t* rgba, uint32_t width, uint32_t height,
          const bake_config& cfg, std::vector<uint8_t>& out_vtex,
          bake_stats* stats = nullptr);

// ============================================================================
// 2.7 Layer 打包:多材质层(albedo/normal/orm)烘焙进单一 VTEX
// ============================================================================

/// 多层烘焙配置:每层一张同尺寸 RGBA8 源图 + 各自通道语义
struct bake_layers_config {
    uint32_t tile_size = 128;
    uint32_t border    = 4;
    channel_kind kinds[4] = {channel_kind::color, channel_kind::normal,
                             channel_kind::scalar, channel_kind::color};
    uint32_t layer_count = 3;          // 1-4
    bool use_bcn       = true;
    bool use_astc      = true;
    bool zlib_tiles    = false;
    float astc_quality = 0.0f;

    bool validate() const {
        return layer_count >= 1 && layer_count <= 4 &&
               tile_size >= 16 && (tile_size & (tile_size - 1)) == 0 &&
               border >= 2 && border <= 16 &&
               ((tile_size + 2 * border) % 4) == 0 &&
               (use_bcn || use_astc);
    }
};

/**
 * @brief 多层烘焙:layer_rgba[i] 为第 i 层源图(全部同尺寸)
 * @details 页索引共享(同一虚拟页网格);数据段按层×格式组织:
 *          [层0 BCn][层0 ASTC][层1 BCn][层1 ASTC]...
 *          header.reserved[0] = layer_count;reserved[1..4] = 每层 bcn_format
 *          (astc_format 全层统一 ASTC_4x4)。采样一次页表查找取全层地址。
 */
bool bake_layers(const uint8_t* const* layer_rgba, uint32_t width, uint32_t height,
                 const bake_layers_config& cfg, std::vector<uint8_t>& out_vtex,
                 bake_stats* stats = nullptr);

} // namespace primal::content::vt
