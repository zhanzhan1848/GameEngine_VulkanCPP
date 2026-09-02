using System;
using System.Buffers.Binary;
using System.Text.Json.Serialization;

namespace Primal.CliFramework;

/// <summary>
/// 帧统计工具：把 RGBA16F（引擎离屏 RT 的原生回读格式，4 个半浮点/像素）
/// 解码并计算标量遥测——每帧均亮度/方差/通道均值。这是帧捕获第 3 层
///（规划 §5.3）的数据底座：闪烁/过曝/渐黑这类多帧故障，先看标量时间序列
/// 再决定要不要拉帧序列。通道无关，离线 test.run 亦复用。
/// </summary>
public static class FrameMetrics
{
    /// <summary>单帧标量遥测。亮度取 Rec.709 加权（渲染验证的通用口径）。</summary>
    public readonly struct LumaStats
    {
        // JSON 契约固定 camelCase（输出契约一致性：CLI/Agent 消费，规划 §6.2）。
        [JsonPropertyName("meanLuma")] public double MeanLuma { get; init; }        // 0..1（>1 表示 HDR 过曝）
        [JsonPropertyName("varianceLuma")] public double VarianceLuma { get; init; }
        [JsonPropertyName("minLuma")] public double MinLuma { get; init; }
        [JsonPropertyName("maxLuma")] public double MaxLuma { get; init; }
        [JsonPropertyName("meanR")] public double MeanR { get; init; }
        [JsonPropertyName("meanG")] public double MeanG { get; init; }
        [JsonPropertyName("meanB")] public double MeanB { get; init; }

        public override string ToString() =>
            $"luma={MeanLuma:F4}±{Math.Sqrt(VarianceLuma):F4} [min={MinLuma:F3} max={MaxLuma:F3}] rgb=({MeanR:F3},{MeanG:F3},{MeanB:F3})";
    }

    /// <summary>
    /// 解码 RGBA16F 像素并统计。像素布局 = 每像素 8 字节，R,G,B,A 依序各 2 字节
    /// 小端半浮点（与 EngineDLL DebugReadRenderTarget 的输出一致）。
    /// </summary>
    public static LumaStats ComputeFromRgba16F(ReadOnlySpan<byte> rgba16f, int pixelCount)
    {
        if (rgba16f.Length < pixelCount * 8)
            throw new ArgumentException($"缓冲区 {rgba16f.Length} 字节不足以容纳 {pixelCount} 像素");

        double sum = 0, sum2 = 0, sumR = 0, sumG = 0, sumB = 0;
        float min = float.MaxValue, max = float.MinValue;
        long counted = 0;

        for (int p = 0; p < pixelCount; p++)
        {
            int o = p * 8;
            float r = HalfToFloat(BinaryPrimitives.ReadUInt16LittleEndian(rgba16f.Slice(o, 2)));
            float g = HalfToFloat(BinaryPrimitives.ReadUInt16LittleEndian(rgba16f.Slice(o + 2, 2)));
            float b = HalfToFloat(BinaryPrimitives.ReadUInt16LittleEndian(rgba16f.Slice(o + 4, 2)));
            // alpha 不参与亮度统计

            // 全 0/NaN 像素仍计数（渐黑故障的表现就是大面积 0）。
            float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            sum += luma; sum2 = sum2 + (double)luma * luma;
            sumR += r; sumG += g; sumB += b;
            if (luma < min) min = luma;
            if (luma > max) max = luma;
            counted++;
        }

        double mean = sum / counted;
        // 总体方差（除 N）：诊断用途足够，且跨帧可比。
        double variance = sum2 / counted - mean * mean;
        return new LumaStats
        {
            MeanLuma = mean,
            VarianceLuma = Math.Max(0, variance),
            MinLuma = min,
            MaxLuma = max,
            MeanR = sumR / counted,
            MeanG = sumG / counted,
            MeanB = sumB / counted,
        };
    }

    /// <summary>IEEE 754 binary16 → float（查表成本不值，位运算 5 步内完成）。</summary>
    internal static float HalfToFloat(ushort half)
    {
        // .NET 的 System.Half 语义正确但走 ICustomFormatter 路径较绕；
        // 直接按位解码，与 EngineDLL SaveRenderTargetAsPng 的口径一致。
        uint sign = (uint)(half & 0x8000) << 16;
        uint exponent = (uint)(half >> 10) & 0x1F;
        uint mantissa = (uint)half & 0x3FF;

        uint bits;
        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign; // ±0
            }
            else
            {
                // 次正规：规格化到 float
                int e = -1;
                uint m = mantissa;
                do { e++; m <<= 1; } while ((m & 0x400) == 0);
                m &= 0x3FF;
                bits = sign | ((uint)(127 - 15 - e) << 23) | (m << 13);
            }
        }
        else if (exponent == 0x1F)
        {
            bits = sign | 0x7F800000u | (mantissa << 13); // Inf/NaN
        }
        else
        {
            bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
        }
        return BitConverter.UInt32BitsToSingle(bits);
    }
}
