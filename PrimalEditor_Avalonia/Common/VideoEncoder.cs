using System;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// PNG 帧序列 → 视频的外部 ffmpeg 编码（CLI 规划 §5.3 第 2 层：视频是帧
/// 序列的输出选项，不是独立能力）。从 FrameCaptureCommands.EncodeVideo 抽取
/// 公共化（Part A sequence.render 与 Part B Run 录制共用）。ffmpeg 是可选
/// 依赖——缺失/失败都不影响 PNG 序列这一主产物，只在 hint 里给出下一步。
/// </summary>
public static class VideoEncoder
{
    /// <summary>候选 ffmpeg 路径/命令名（homebrew → 系统 → PATH）。</summary>
    public static readonly string[] FfmpegCandidates =
    {
        "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "ffmpeg",
    };

    /// <summary>找到可用的 ffmpeg；null = 未安装。</summary>
    public static string? FindFfmpeg() =>
        FfmpegCandidates.FirstOrDefault(Findable);

    /// <summary>
    /// 构造 ffmpeg 参数（纯函数，供单测）。帧序列约定 frame_%04d.png 从 0 起
    /// （editor.record / sequence.render / PrimalPlayer --record 同款命名）；
    /// 宽高钳成偶数（H.264 yuv420p 约束），crf 20 视觉无损级。
    /// </summary>
    public static string BuildArguments(string framesDir, string videoPath, int width, int height, int framerate)
    {
        var evenW = width - (width % 2);
        var evenH = height - (height % 2);
        var fps = Math.Clamp(framerate, 1, 240);
        return "-y -framerate " + fps + " -i \"" + Path.Combine(framesDir, "frame_%04d.png") + "\"" +
               " -vf scale=" + evenW + ":" + evenH +
               " -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + videoPath + "\"";
    }

    /// <summary>编码 framesDir 下的 frame_%04d.png 为视频。返回 (是否成功, 失败提示)。</summary>
    public static (bool Encoded, string? Hint) EncodePngSequence(
        string framesDir, string videoPath, int width, int height, int framerate)
    {
        var ffmpeg = FindFfmpeg();
        if (ffmpeg == null)
            return (false, "ffmpeg 未安装——PNG 序列已保留，安装后可重试编码（brew install ffmpeg）");

        var videoDir = Path.GetDirectoryName(Path.GetFullPath(videoPath));
        if (!string.IsNullOrEmpty(videoDir)) Directory.CreateDirectory(videoDir);

        var psi = new System.Diagnostics.ProcessStartInfo
        {
            FileName = ffmpeg,
            Arguments = BuildArguments(framesDir, videoPath, width, height, framerate),
            UseShellExecute = false,
            RedirectStandardError = true,
        };
        try
        {
            using var process = System.Diagnostics.Process.Start(psi);
            if (process == null) return (false, "ffmpeg 进程启动失败");
            var stderr = process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0 || !File.Exists(videoPath))
            {
                var lastLine = stderr.Split('\n').LastOrDefault(l => l.Trim().Length > 0) ?? "";
                return (false, $"ffmpeg 退出码 {process.ExitCode}：{lastLine[..Math.Min(160, lastLine.Length)]}");
            }
            return (true, null);
        }
        catch (Exception ex)
        {
            return (false, ex.Message);
        }
    }

    private static bool Findable(string candidate)
    {
        if (candidate.Contains('/')) return File.Exists(candidate);
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var dir in path.Split(':'))
            if (File.Exists(Path.Combine(dir, candidate))) return true;
        return false;
    }
}
