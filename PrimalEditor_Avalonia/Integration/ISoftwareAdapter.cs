using System.Collections.Generic;

namespace PrimalEditor_Avalonia.Integration;

/// <summary>
/// 软件适配器契约（S1：只做静态发现——声明候选入口；不启动进程、不联网、
/// 不占许可证。probe/超时/取消是 S2 的 detect/probe 分离扩展点）。
/// 扩展纪律：新增软件 = 新适配器 + 注册，不改中心核心（S3 协议的雏形）。
/// </summary>
public interface ISoftwareAdapter
{
    /// <summary>稳定软件 ID（SoftwareRecord.SoftwareId 同值）。</summary>
    string Id { get; }
    /// <summary>类别（video-encode / dcc / cloud / ...）。</summary>
    string Category { get; }
    /// <summary>true = 远端服务（S1 只登记 IsRemote，不做连通探测）。</summary>
    bool IsRemote { get; }
    /// <summary>适配器版本（记录进 SoftwareRecord，迁移规则依据）。</summary>
    int AdapterVersion { get; }
    /// <summary>静态候选入口（绝对路径或 PATH 命令名）。顺序即优先级。</summary>
    IReadOnlyList<string> StaticCandidates();
}

/// <summary>
/// FFmpeg 适配器——候选从 Common/VideoEncoder.cs 迁移至此（单一事实源），
/// VideoEncoder.FfmpegCandidates 转发引用，编码路径行为不变。
/// </summary>
public sealed class FfmpegAdapter : ISoftwareAdapter
{
    /// <summary>候选 ffmpeg 路径/命令名（homebrew → 系统 → PATH）。</summary>
    public static readonly string[] Candidates =
    {
        "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "ffmpeg",
    };

    public string Id => "ffmpeg";
    public string Category => "video-encode";
    public bool IsRemote => false;
    public int AdapterVersion => 1;
    public IReadOnlyList<string> StaticCandidates() => Candidates;
}
