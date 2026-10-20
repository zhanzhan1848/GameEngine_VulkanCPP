// LightmapPagePng —— 烘焙页可视化 PNG 导出（见 .h 契约）。
//
// HDR→LDR 映射（与页面数据本身无关，纯查看用）：
//   Reinhard（c/(1+c)，白点 1.0 归一前的场景值直接压）→ gamma 2.2。
// 选 Reinhard 而非简单曝光：烘焙值跨多个数量级（直射弹射 vs 暗部），
// 简单 ×曝光 会截顶丢失暗部层次；Reinhard 无参且单调保序。
//
// stb_image_write 以 STB_IMAGE_WRITE_STATIC 引入（内部链接）——
// EngineTest 多个测试 TU 自带 stb 实现且链接 libEngine，外部链接的
// 重复定义会炸链接；static 隔离后各 TU 互不可见。

#include "LightmapPagePng.h"

#include <cmath>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::baking {

bool WriteLightmapPagePng(const std::string& path,
                          const float* rgb,
                          std::uint32_t width, std::uint32_t height,
                          const std::uint32_t* rectsXYWH, std::uint32_t rectCount) {
    if (!rgb || width == 0 || height == 0) return false;

    // 1) HDR → LDR：Reinhard + gamma 2.2。
    std::vector<unsigned char> px((size_t)width * height * 3);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = rgb[i * 3 + c];
            const float tone = v / (1.0f + v);              // Reinhard
            float enc = tone <= 0.0f ? 0.0f : std::pow(tone, 1.0f / 2.2f);
            if (enc > 1.0f) enc = 1.0f;
            px[i * 3 + c] = static_cast<unsigned char>(enc * 255.0f + 0.5f);
        }
    }

    // 2) per-mesh 矩形 1px 白框（packer rect，texel 单位；边界夹在图集内）。
    auto plot = [&](std::int32_t x, std::int32_t y) {
        if (x < 0 || y < 0 || x >= (std::int32_t)width || y >= (std::int32_t)height) return;
        const size_t i = ((size_t)y * width + x) * 3;
        px[i] = px[i + 1] = px[i + 2] = 255;
    };
    for (std::uint32_t r = 0; r < rectCount && rectsXYWH; ++r) {
        const std::int32_t x0 = (std::int32_t)rectsXYWH[r * 4 + 0];
        const std::int32_t y0 = (std::int32_t)rectsXYWH[r * 4 + 1];
        const std::int32_t w = (std::int32_t)rectsXYWH[r * 4 + 2];
        const std::int32_t h = (std::int32_t)rectsXYWH[r * 4 + 3];
        if (w <= 0 || h <= 0) continue;
        for (std::int32_t dx = 0; dx < w; ++dx) {
            plot(x0 + dx, y0);
            plot(x0 + dx, y0 + h - 1);
        }
        for (std::int32_t dy = 0; dy < h; ++dy) {
            plot(x0, y0 + dy);
            plot(x0 + w - 1, y0 + dy);
        }
    }

    // 3) 落盘（PNG， stride=width*3）。
    const int stride = (int)width * 3;
    if (!stbi_write_png(path.c_str(), (int)width, (int)height, 3, px.data(), stride)) {
        PRIMAL_LOG_STREAM(Error, "Baking") << "[Lightmap] page PNG export failed: " << path << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "Baking")
        << "[Lightmap] page PNG exported: " << path << " (" << width << "x" << height
        << ", Reinhard+gamma2.2, " << rectCount << " mesh rects)" << std::endl;
    return true;
}

} // namespace primal::graphics::baking
