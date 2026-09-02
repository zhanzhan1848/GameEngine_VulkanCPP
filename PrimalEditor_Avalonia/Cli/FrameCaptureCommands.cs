using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Views.Engine3D;
using System.Threading.Tasks;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 帧捕获三层（CLI 规划 §5.3）：metrics（标量遥测，成本最低，agent 排查
/// 多帧问题的首选入口）→ record（帧序列落盘）→ diff（基线/帧间对比，
/// 自动测试共用）。设计约束：帧数据走文件系统/回读缓冲，JSON 消息只带
/// 元数据与标量；全部帧操作持有视口互锁，不卡渲染主循环。
/// </summary>
public sealed class FrameCaptureCommands
{
    private const int MaxFrames = 240;

    [CliCommand("editor.record",
        Description = "连续渲染 N 帧并逐帧存 PNG 到目录（frame_0000.png 起）。用于多帧故障取证" +
                      "（闪烁/撕裂/渐黑）与「看→改→验证」的时域证据。帧间引擎由渲染 tick 正常推进，" +
                      "每帧真实耗时在 frameMs 中。frames ≤ 240；out 必须为绝对路径（目录自动创建）。" +
                      "3D 视口未初始化返回 NOT_READY。",
        Example = """{"frames":30,"out":"/tmp/record"}""")]
    public object Record(int frames = 30, string @out = "")
    {
        if (frames is < 1 or > MaxFrames)
            throw CliError.InvalidParams($"frames 取值 1..{MaxFrames}", "诊断场景 30 帧通常足够定位闪烁");
        if (string.IsNullOrWhiteSpace(@out))
            throw CliError.InvalidParams("out 不能为空", @"示例: {""frames"":30,""out"":""/tmp/record""}");
        var dir = ResolveDirectory(@out);

        var viewport = RequireViewport();
        if (!viewport.TryRecordFrames(frames, dir, out var files, out var width, out var height, out var frameMs))
            throw new CliError(CliErrorCodes.EngineError,
                "帧序列捕获失败（视口未完成首次渲染或引擎忙）",
                "稍候重试；已写出的帧保留在输出目录可供取证");

        return new
        {
            dir = dir,
            frames = files.Count,
            width,
            height,
            avgFrameMs = Math.Round(frameMs.Count == 0 ? 0 : frameMs.Average(), 2),
            files,
        };
    }

    [CliCommand("editor.metrics",
        Description = "连续渲染 N 帧并输出每帧标量遥测（Rec.709 均亮度/方差/min/max/通道均值，HDR 原值不截断）。" +
                      "排查闪烁/过曝/渐黑的首选入口——标量序列成本比图像回读低几个量级，" +
                      "先看它异常再 editor.record 拉帧序列。frames ≤ 240，耗时约 frames×(渲染+回读)。" +
                      "3D 视口未初始化返回 NOT_READY。",
        Example = """{"frames":60}""")]
    public async Task<object> Metrics(int frames = 30)
    {
        if (frames is < 1 or > MaxFrames)
            throw CliError.InvalidParams($"frames 取值 1..{MaxFrames}");
        var viewport = RequireViewport();

        var samples = new List<object>(frames);
        var captureMs = new List<double>(frames);
        int width = 0, height = 0;
        for (int i = 0; i < frames; i++)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            if (!viewport.TryReadFramePixels(out var rgba, out width, out height))
                throw new CliError(CliErrorCodes.EngineError,
                    $"第 {i} 帧读回失败（视口未就绪或引擎忙）",
                    "先 editor.ping 确认 engineInitialized，稍候重试");
            var stats = FrameMetrics.ComputeFromRgba16F(rgba, width * height);
            captureMs.Add(sw.Elapsed.TotalMilliseconds);
            samples.Add(new
            {
                index = i,
                captureMs = Math.Round(captureMs[^1], 2),
                meanLuma = stats.MeanLuma,
                varianceLuma = stats.VarianceLuma,
                minLuma = stats.MinLuma,
                maxLuma = stats.MaxLuma,
                meanR = stats.MeanR,
                meanG = stats.MeanG,
                meanB = stats.MeanB,
            });

            // 让出 UI 线程一拍：排空 socket 队列里等编组的编辑命令——
            // 否则整个采样循环独占主线程，"注入闪烁 → 遥测观察"的闭环
            // 会退化成"先编辑完再采样"（DoD #6 实验中发现）。
            await Avalonia.Threading.Dispatcher.UIThread.InvokeAsync(() => { });
        }

        return new
        {
            frames = samples.Count,
            width,
            height,
            totalMs = Math.Round(captureMs.Sum(), 1),
            samples,
        };
    }

    [CliCommand("editor.diff",
        Description = "逐像素对比两张 PNG（RGB 通道差），输出 meanAbsDelta/maxAbsDelta/changedPixelRatio" +
                      "（任一通道差 > threshold 的像素占比，threshold 默认 8/255）。" +
                      "out 可选：写出差异热图（黑=相同，亮度=差异，红黄白=幅度递增），" +
                      "把「哪里在变」直接可视化。尺寸不同时 sameSize=false。" +
                      "自动测试的基线对比与 agent 改前/改后对比共用本命令。",
        Example = """{"a":"/tmp/before.png","b":"/tmp/after.png","out":"/tmp/diff.png","threshold":8}""")]
    public object Diff(string a, string b, string? @out = null, int threshold = ImageDiff.DefaultThreshold)
    {
        if (string.IsNullOrWhiteSpace(a) || string.IsNullOrWhiteSpace(b))
            throw CliError.InvalidParams("a/b 两个图像路径都不能为空");
        if (threshold is < 0 or > 255)
            throw CliError.InvalidParams("threshold 取值 0..255");
        if (!File.Exists(a)) throw CliError.NotFound($"图像 {a}", "确认路径；截图用 editor.screenshot");
        if (!File.Exists(b)) throw CliError.NotFound($"图像 {b}", "确认路径；截图用 editor.screenshot");

        string? heatmap = null;
        if (!string.IsNullOrWhiteSpace(@out))
        {
            heatmap = Path.IsPathRooted(@out) ? @out : Path.GetFullPath(@out);
        }

        var result = ImageDiff.Compare(a, b, heatmap, threshold);
        return new
        {
            sameSize = result.SameSize,
            identical = result.Identical,
            sizeA = new[] { result.WidthA, result.HeightA },
            sizeB = new[] { result.WidthB, result.HeightB },
            meanAbsDelta = result.MeanAbsDelta,
            maxAbsDelta = result.MaxAbsDelta,
            changedPixelRatio = result.ChangedPixelRatio,
            changedPixelCount = result.ChangedPixelCount,
            threshold,
            heatmap,
            // 自动测试判定口径：identical=严格逐像素一致；changedPixelRatio ≤ 容差即通过
            verdict = result.Identical ? "identical"
                : !result.SameSize ? "different-size"
                : result.ChangedPixelRatio == 0 ? "within-threshold" : "different",
        };
    }

    private static EngineViewport3D RequireViewport() =>
        EditorServices.FindViewport()
        ?? throw CliError.NotReady("3D 视口未就绪",
            "在编辑器中打开一个项目并切换到世界编辑器标签后再捕获");

    private static string ResolveDirectory(string path)
    {
        var dir = Path.IsPathRooted(path) ? path : Path.GetFullPath(path);
        Directory.CreateDirectory(dir);
        return dir;
    }
}
