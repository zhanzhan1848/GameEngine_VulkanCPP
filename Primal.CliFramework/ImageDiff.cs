using System;
using System.IO;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;

namespace Primal.CliFramework;

/// <summary>
/// 图像对比（帧捕获第 3 层的 diff 动词底座，规划 §5.3）：两张 PNG 的逐像素差，
/// 产出标量指标 + 差异热图。自动测试的基线对比与 agent 的「改前/改后」对比共用。
/// 尺寸不同 → sameSize=false（基线对比里尺寸变化本身就是结果）。
/// </summary>
public static class ImageDiff
{
    public const int DefaultThreshold = 8; // 0..255，单通道差超过它视为「变化像素」

    public sealed record Result
    {
        public bool Identical { get; init; }
        public bool SameSize { get; init; }
        public int WidthA { get; init; }
        public int HeightA { get; init; }
        public int WidthB { get; init; }
        public int HeightB { get; init; }
        public double MeanAbsDelta { get; init; }       // 全图像素平均绝对差（RGB 均）
        public double MaxAbsDelta { get; init; }
        public double ChangedPixelRatio { get; init; }  // 任一通道差 > threshold 的像素占比
        public int ChangedPixelCount { get; init; }
    }

    /// <summary>
    /// 对比两张 PNG。heatmapPath 可选——存在则写出差异热图（黑=相同，
    /// 亮度=差异幅度，热色映射便于「哪里在变」一眼可见）。
    /// </summary>
    public static Result Compare(string pathA, string pathB, string? heatmapPath = null,
        int threshold = DefaultThreshold)
    {
        using var a = Image.Load<Rgba32>(pathA);
        using var b = Image.Load<Rgba32>(pathB);

        var result = new Result
        {
            SameSize = a.Width == b.Width && a.Height == b.Height,
            WidthA = a.Width, HeightA = a.Height,
            WidthB = b.Width, HeightB = b.Height,
        };
        if (!result.SameSize)
            return result;

        int width = a.Width, height = a.Height;
        long pixelCount = (long)width * height;
        var pixelsA = new Rgba32[pixelCount];
        var pixelsB = new Rgba32[pixelCount];
        a.CopyPixelDataTo(pixelsA);
        b.CopyPixelDataTo(pixelsB);
        Rgba32[]? heatPixels = heatmapPath != null ? new Rgba32[pixelCount] : null;

        long sumDelta = 0;
        int maxDelta = 0;
        int changedCount = 0;

        for (long p = 0; p < pixelCount; p++)
        {
            var ca = pixelsA[p];
            var cb = pixelsB[p];
            int dr = Math.Abs(ca.R - cb.R);
            int dg = Math.Abs(ca.G - cb.G);
            int db = Math.Abs(ca.B - cb.B);
            int delta = Math.Max(dr, Math.Max(dg, db));
            sumDelta += dr + dg + db;
            if (delta > maxDelta) maxDelta = delta;
            bool changed = delta > threshold;
            if (changed) changedCount++;
            if (heatPixels != null)
                heatPixels[p] = changed ? HeatColor(delta) : new Rgba32(0, 0, 0, 255);
        }

        result = result with
        {
            Identical = maxDelta == 0,
            MeanAbsDelta = (double)sumDelta / (pixelCount * 3),
            MaxAbsDelta = maxDelta,
            ChangedPixelRatio = (double)changedCount / pixelCount,
            ChangedPixelCount = changedCount,
        };

        if (heatPixels != null)
        {
            using var heatmap = Image.LoadPixelData<Rgba32>(heatPixels, width, height);
            var dir = Path.GetDirectoryName(heatmapPath!);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            heatmap.SaveAsPng(heatmapPath!);
        }
        return result;
    }

    private static Rgba32 HeatColor(int delta)
    {
        // 黑→红→黄→白 的简易热色带：差异越大越亮。
        int d = Math.Clamp(delta, 0, 255);
        byte r = (byte)Math.Min(255, d * 3);
        byte g = (byte)Math.Max(0, Math.Min(255, d * 3 - 255));
        byte b = (byte)Math.Max(0, d * 3 - 510);
        return new Rgba32(r, g, b, 255);
    }
}
