/**
 * @file NeuralWeights.h
 * @brief .pnn 神经网络权重资产解析 + CPU 参考前向（零渲染依赖，可 Unit 单测）
 *
 * M0 神经渲染基础设施 Layer B 的纯数据半边。GPU 侧组合（权重 → SSBO →
 * compute pipeline）在 NeuralRuntime；这里只做字节 → 数学。
 *
 * .pnn v1 布局（全部 little-endian，与 ContentTools/neural/train_image_fit.py
 * 导出侧逐字节镜像；hash/前向数值语义见文件尾的 PRIMAL NEURAL SPEC v1 注释，
 * 同一份规格在两个 debug shader 与 Python 训练器各有一份拷贝——改动必须四处同步）：
 *
 *   offset 0:   u32 magic = 0x31574E50  ("PNW1" LE)
 *   offset 4:   u32 version = 1
 *   offset 8:   u32 layer_count
 *   offset 12:  u32 table_element_count   (f32 个数；hash grid 参数表，可为 0)
 *   offset 16:  u32 input_dim
 *   offset 20:  u32 output_dim
 *   offset 24:  u32 param_count           (u32 编码参数个数 = 16 级分辨率)
 *   offset 28:  u32 reserved = 0
 *   offset 32:  u32 level_res[param_count]
 *   ...         LayerDesc[layer_count] { u32 in_dim; u32 out_dim; u8 activation;
 *                                        u8 quant; u16 pad }   (8 B/条)
 *   ...         f32 table[table_element_count]
 *   ...         每层: f32 W[out_dim][in_dim] (row-major) + f32 b[out_dim]
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace primal::graphics::neural {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using f32 = float;

constexpr u32 kPnnMagic = 0x31574E50;   // "PNW1" little-endian
constexpr u32 kPnnVersion = 1;
constexpr u32 kPnnHeaderSize = 32;

enum class Activation : u8 {
    None = 0,
    Relu = 1,
    Sigmoid = 2,
};

enum class LayerKind : u8 {
    Dense = 0,
    Conv = 1,   // 3×3，stride 1；pad 语义见 flags/注释
};

/// 层描述。v1 层 = Dense 12B 条目；v2 层 = 24B 条目（含 conv 字段）。
struct LayerDesc {
    u32 in_dim = 0;     // dense: 特征维；conv: 输入通道数
    u32 out_dim = 0;    // dense: 输出维；conv: 输出通道数
    Activation activation = Activation::None;
    u8 quant = 0;       // 0 = f32（v1/v2 仅支持 f32）
    // v2 conv 扩展（v1 层全零）
    LayerKind kind = LayerKind::Dense;
    u8 kernel = 0;      // conv: 3
    u8 stride = 0;      // conv: 1
    u8 pad = 0;         // conv: 1（引擎/CPU 参考语义 = clamp-to-edge；torch 导出为
                        // zero-pad，parity 单测只比较内部像素）
    u32 flags = 0;      // bit0 = expm1：层输出 = expm1(x)（降噪器末层出线性 HDR）
    // 权重在 weights_ 里的 f32 偏移（解析时推导，不落盘）。
    u32 w_offset = 0;   // dense: W[out][in]；conv: W[out][in][k*k]（row-major）
    u32 b_offset = 0;   // bias 起点
};

/// 解析后的网络。table + weights 都是 f32 平铺数组，可直接整块上传 SSBO。
struct NeuralNetwork {
    u32 version = 0;
    u32 flags = 0;      // v2 头部 reserved 槽复用：bit0 = residual_rgb
                         // （网络输出需加输入 rgb —— 降噪器残差结构）
    u32 input_dim = 0;
    u32 output_dim = 0;
    std::vector<LayerDesc> layers;
    std::vector<u32> level_res;   // hash grid 每级分辨率（编码用，可为空）
    std::vector<f32> table;       // hash grid 参数（L*T*F f32，可为空）
    std::vector<f32> weights;     // 逐层 [W][b] 连续平铺
};

/// 解析 .pnn 字节流。失败返回 false 并把原因写入 *err（可空）。
bool ParsePnn(const u8* data, size_t size, NeuralNetwork& out, std::string* err);

/// 文件加载便捷封装（内部读全量后走 ParsePnn）。
bool LoadPnnFile(const std::string& path, NeuralNetwork& out, std::string* err);

/// CPU 参考前向：hash-encode(u,v) + 逐层前向。uv 为 2 f32，out 至少 output_dim。
/// scratch 供内部暂存（容量 ≥ input_dim 与最大 out_dim），跨帧复用避免重分配。
/// 数值语义 = GPU kernel（同序累加），用于对拍门禁；不是优化路径。
bool ReferenceForward(const NeuralNetwork& net, const f32* uv2, f32* out,
                      std::vector<f32>& scratch);

/// CPU 参考 conv 前向（v2 网络）：输入/输出为平面布局 [c][y][x]（通道外层、
/// 行主序），planes_in 至少 in_channels*w*h f32。边界 = clamp-to-edge。
/// flags bit0 的末层输出 expm1。仅比较/产出的像素范围由调用方决定。
bool ReferenceForwardConv(const NeuralNetwork& net, const f32* planes_in,
                          u32 width, u32 height, f32* planes_out,
                          std::vector<f32>& scratch);

/// 仅 hash 编码（parity 向量的编码级对拍用）。feat 至少 input_dim。
bool HashEncode(const NeuralNetwork& net, f32 u, f32 v, f32* feat);

}  // namespace primal::graphics::neural
