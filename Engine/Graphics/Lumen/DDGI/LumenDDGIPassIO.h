/**
 * @file LumenDDGIPassIO.h
 * @brief LumenDDGIPass 的 shader 装载与诊断读回辅助（独立 TU）
 *
 * R3（Windows 首编，MSVC C1001 第三轮）：LumenDDGIPass.cpp 是 4834 行巨 TU
 * （AddPass 单函数 ~1780 行）。MSVC Release 仍 C1001 且 /Od（后端降优化）
 * 无效时，高度怀疑前端（c1.dll）压力——把与类状态无关的 IO 辅助搬出为
 * 独立 TU，缩小主 TU 的前端负担与 ICE 排查面。代码为逐字搬移（仅去
 * static、改经本头导出），行为零变化；macOS 侧由全量构建+DDGI 单测回归。
 *
 * 依赖边界：只依赖 RHI Core / ShaderRegistry / Dawn ShaderLoader，不访问
 * LumenDDGIPass 私有状态。
 */
#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

#include <string>
#include <vector>

namespace primal::graphics::rhi {
class RHIDeviceBase;
}

namespace primal::graphics::lumen {

struct DDGIRuntimeParams;
struct DDGIVolumeData;

/// 按设备平台装载 DDGI shader 字节码（Metal 源码+#include 内联 / Vulkan
/// SPIR-V / Dawn WGSL，探测序列与降级语义见 .cpp）。失败返回空并写日志。
std::vector<u8> LoadShaderBytecode(const char* shaderName, rhi::RHIDeviceBase* device);

/// DDGI_ORIGIN_STATS=1 一次性诊断：同步读回 relocated origins，统计重定位
/// 分类分布与位移分位数（同步停等仅诊断路径）。total==0 或句柄无效时 no-op。
void DumpRelocatedOriginStats(rhi::RHIDeviceBase* device, rhi::ResourceHandle originsBuf,
                              const DDGIRuntimeParams& params, const DDGIVolumeData& volume);

}  // namespace primal::graphics::lumen
