#pragma once

// 烘焙页可视化 PNG 导出（2026-10-20 用户需求 3）。
//
// 烘焙落盘（lightmap.lmap）的同时，把图集页导出为人工可查看的
// lightmap_page.png：RGBA16F 线性辐射值 → LDR（Reinhard tonemap +
// gamma 2.2，见 .cpp 头注释），并叠加 per-mesh 矩形 1px 白框（packer
// 的 rect，texel 单位）。导出失败只打日志、不阻断烘焙（可视化产物，
// 非数据链路）。

#include <string>
#include <cstdint>

namespace primal::graphics::baking {

// rgb：w*h*3 个 float（线性 HDR，行主序）；rectsXYWH：rectCount*4 个
// uint32（x,y,w,h，texel 单位，可为 nullptr/0）。成功返回 true。
bool WriteLightmapPagePng(const std::string& path,
                          const float* rgb,
                          std::uint32_t width, std::uint32_t height,
                          const std::uint32_t* rectsXYWH, std::uint32_t rectCount);

} // namespace primal::graphics::baking
