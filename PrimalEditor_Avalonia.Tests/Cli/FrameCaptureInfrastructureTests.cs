using System;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

public sealed class FrameCaptureInfrastructureTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "primal_cli_tests_" + Guid.NewGuid().ToString("N"));

    public FrameCaptureInfrastructureTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch { }
    }

    [Theory]
    [InlineData(0x0000, 0f)]          // +0
    [InlineData(0x8000, -0f)]         // -0
    [InlineData(0x3C00, 1f)]          // 1.0
    [InlineData(0xC000, -2f)]         // -2.0
    [InlineData(0x4000, 2f)]          // 2.0
    [InlineData(0x3555, 0.33325195f)] // ≈1/3
    public void HalfToFloat_MatchesKnownBinary16Values(ushort half, float expected)
    {
        Assert.Equal(expected, FrameMetrics.HalfToFloat(half), 5);
    }

    [Fact]
    public void ComputeFromRgba16F_SolidGrayFrameHasZeroVariance()
    {
        // 2x1 像素，RGB 均 0.5：luma = 0.5，方差 0，通道均值一致。
        // 缓冲内容就是半浮点位型（0x3800 = 0.5），小端字节序。
        var rgba = new byte[2 * 8];
        ushort[] bits = { 0x3800, 0x3800, 0x3800, 0x3C00 }; // R G B A = 0.5,0.5,0.5,1.0
        for (int p = 0; p < 2; p++)
            for (int c = 0; c < 4; c++)
                BitConverter.GetBytes(bits[c]).CopyTo(rgba, p * 8 + c * 2);

        var stats = FrameMetrics.ComputeFromRgba16F(rgba, 2);

        Assert.Equal(0.5, stats.MeanLuma, 4);
        Assert.Equal(0, stats.VarianceLuma, 6);
        Assert.Equal(0.5, stats.MeanR, 4);
        Assert.Equal(0.5, stats.MeanG, 4);
        Assert.Equal(0.5, stats.MeanB, 4);
        Assert.InRange(stats.MaxLuma, 0.4999, 0.5001);
    }

    [Fact]
    public void ComputeFromRgba16F_ContrastingPixelsProduceVariance()
    {
        // 黑像素 + 白像素：luma 均值 0.5，方差 0.25。
        var black = new byte[] { 0, 0, 0, 0, 0, 0, 0, 0 };
        var white = new byte[] { 0, 0x3C, 0, 0x3C, 0, 0x3C, 0, 0x3C }; // 1.0 ×4（小端半浮点）
        var rgba = black.Concat(white).ToArray();

        var stats = FrameMetrics.ComputeFromRgba16F(rgba, 2);

        Assert.Equal(0.5, stats.MeanLuma, 4);
        Assert.Equal(0.25, stats.VarianceLuma, 4);
        Assert.Equal(0, stats.MinLuma, 4);
        Assert.Equal(1, stats.MaxLuma, 4);
    }

    [Fact]
    public void ImageDiff_IdenticalMaxDeltaAndMetrics()
    {
        var (aPath, bPath) = WritePair(
            new Rgba32(100, 100, 100, 255),
            new Rgba32(100, 100, 100, 255));

        var result = ImageDiff.Compare(aPath, bPath);

        Assert.True(result.Identical);
        Assert.Equal(0, result.MaxAbsDelta);
        Assert.Equal(0, result.ChangedPixelCount);
    }

    [Fact]
    public void ImageDiff_ChangedPixelsCountedAndHeatmapWritten()
    {
        // 2x1：像素0 全通道 +30（超默认阈值 8），像素1 不变。
        var (aPath, bPath) = WritePair(
            new Rgba32(100, 100, 100, 255),
            new Rgba32(100, 130, 100, 255),
            secondPixel: new Rgba32(70, 70, 70, 255));
        var heatmap = Path.Combine(_dir, "sub", "heat.png");

        var result = ImageDiff.Compare(aPath, bPath, heatmap);

        Assert.False(result.Identical);
        Assert.Equal(30, result.MaxAbsDelta);
        Assert.Equal(1, result.ChangedPixelCount);
        Assert.Equal(0.5, result.ChangedPixelRatio, 6);
        Assert.True(File.Exists(heatmap), "热图未写出");
        Assert.True(new FileInfo(heatmap).Length > 0);
    }

    [Fact]
    public void ImageDiff_SizeMismatchReportsSameSizeFalse()
    {
        var aPath = Path.Combine(_dir, "a.png");
        var bPath = Path.Combine(_dir, "b.png");
        MakeImage(aPath, 2, 1, new Rgba32(1, 2, 3, 255));
        MakeImage(bPath, 1, 1, new Rgba32(1, 2, 3, 255));

        var result = ImageDiff.Compare(aPath, bPath);

        Assert.False(result.SameSize);
        Assert.Equal((2, 1), (result.WidthA, result.HeightA));
        Assert.Equal((1, 1), (result.WidthB, result.HeightB));
    }

    private (string A, string B) WritePair(Rgba32 firstA, Rgba32 firstB, Rgba32? secondPixel = null)
    {
        var aPath = Path.Combine(_dir, "a.png");
        var bPath = Path.Combine(_dir, "b.png");
        MakeImage(aPath, 2, 1, firstA, secondPixel);
        MakeImage(bPath, 2, 1, firstB, secondPixel);
        return (aPath, bPath);
    }

    private static void MakeImage(string path, int width, int height, Rgba32 first, Rgba32? second = null)
    {
        using var image = new Image<Rgba32>(width, height);
        image[0, 0] = first;
        if (second.HasValue && width > 1)
            image[1, 0] = second.Value;
        image.SaveAsPng(path);
    }
}
