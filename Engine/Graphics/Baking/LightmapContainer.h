#pragma once

// LMAP 烘焙容器（2026-10-19 lightmap 计划，M1 契约冻结）。
//
// 单文件 = 页数据 + per-mesh 矩形清单（scale/bias 契约的唯一载体）。
// 布局小端、定长头 + 定长 mesh 条目 + 原始纹素，读写零外部依赖
// （BC6H 等压缩格式走 format 枚举预留，编码器到位前只用 RGBA16F）。
//
//   Header（定长 40B）:
//     u32 magic  'LMAP' = 0x50414D4C
//     u32 version = 1
//     u32 width, height              // 页尺寸（v1 单页方形）
//     u32 format                     // 0=RGBA16F(f16×4) 1=BC6H(reserved)
//     u32 atlas_count                // v1 恒 1（契约预留多页）
//     u32 mesh_count
//     u32 texel_bytes                // w*h*8*atlas_count（读侧完整性校验）
//   MeshEntry（定长 104B × mesh_count）:
//     char name[64]                  // 场景内 mesh 键（零填充）
//     u32  atlas_index               // v1 = 0
//     f32  scale_x, scale_y, bias_x, bias_y   // uv_atlas = uv*scale + bias
//     u32  rect_x, rect_y, rect_w, rect_h     // 分配矩形（texel）
//     u32  reserved                 // 零
//   Data: f16 RGBA 交织纹素，页连续。

#include <cstdint>
#include <string>
#include <vector>

namespace primal::graphics::baking {

constexpr std::uint32_t kLightmapMagic = 0x50414D4C;  // 'LMAP' little-endian
constexpr std::uint32_t kLightmapVersion = 1;
constexpr std::uint32_t kLightmapFormatRGBA16F = 0;
constexpr std::uint32_t kLightmapFormatBC6H = 1;  // 预留：编码器到位前不写

constexpr std::uint32_t kLightmapNameLength = 64;

struct LightmapMeshEntry {
    char name[kLightmapNameLength]{};
    std::uint32_t atlas_index = 0;
    float scale_x = 1.0f, scale_y = 1.0f;
    float bias_x = 0.0f, bias_y = 0.0f;
    std::uint32_t rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    std::uint32_t reserved = 0;

    void SetName(const std::string& n) {
        std::uint32_t i = 0;
        for (; i < kLightmapNameLength - 1 && i < n.size(); ++i) name[i] = n[i];
        for (; i < kLightmapNameLength; ++i) name[i] = 0;
    }
    std::string GetName() const { return std::string(name, strnlen(name, kLightmapNameLength)); }
};
static_assert(sizeof(LightmapMeshEntry) == 104, "LMAP mesh entry layout frozen (ABI 镜像锁尺寸)");

struct LightmapPageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = kLightmapFormatRGBA16F;
    std::vector<std::uint16_t> texels_rgba16f;  // width*height*4（f16 交织）
};

struct LightmapContainer {
    std::uint32_t atlas_count = 1;
    std::vector<LightmapPageData> pages;
    std::vector<LightmapMeshEntry> entries;
};

// 写容器（覆盖写）。返回 false = IO 失败或内容自检失败（尺寸/纹素量不符）。
bool SaveLightmapContainer(const std::string& path, const LightmapContainer& c);

// 读容器。返回 false = 文件缺失 / 坏 magic / 坏版本 / 截断 / 校验不符。
bool LoadLightmapContainer(const std::string& path, LightmapContainer& out);

} // namespace primal::graphics::baking
