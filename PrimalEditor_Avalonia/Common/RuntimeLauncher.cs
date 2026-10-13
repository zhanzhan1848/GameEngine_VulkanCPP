using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Run 按钮的新语义（规划 §Run/Stop，2026-09-03）：导出运行时场景包 → 启动
/// 外部宿主 PrimalPlayer。分图形API/分 debug/release = 选择不同构建树/配置
/// 产出的宿主二进制（引擎单例决定一进程一份 EngineDLL，宿主必须独立进程）。
/// Stop 沿用 BuildService 的进程句柄管理。
/// </summary>
public static class RuntimeLauncher
{
    /// <summary>
    /// Run 的三条路线（RunModeIndex 与工具栏 ComboBox 一一对应）：
    ///   PlayerWindow      —— 原路线：导出运行时包 → 外部 PrimalPlayer 默认窗口；
    ///   FixedResolution   —— 同样导出 + 外部 PrimalPlayer，但 --width/--height/--fixed
    ///                        强制窗口内容恰为指定分辨率且不可拖拽缩放（录制/像素比对
    ///                        需要确定性输出时用）；
    ///   RenderSurface     —— 不开外部进程：编辑器内嵌 render surface 切到 runtime
    ///                        渲染路径（PipelineSetEditorMode(false)）并透过场景相机取景，
    ///                        由 EngineViewport3D.BeginRuntimePreview 实现。
    /// </summary>
    public enum RunMode
    {
        PlayerWindow = 0,
        FixedResolution = 1,
        RenderSurface = 2,
    }

    /// <summary>VM 的 RunModeIndex（0..2）→ RunMode；越界值收敛到 PlayerWindow。</summary>
    public static RunMode ResolveMode(int runModeIndex)
    {
        var clamped = Math.Clamp(runModeIndex, 0, 2);
        return (RunMode)clamped;
    }

    /// <summary>(graphicsApi, buildConfig) → 构建树目录（repo 相对）。
    /// Vulkan 树目前只有 Debug 配置产物（Darwin/DebugVulkan）。</summary>
    public static string? ResolvePlayerDir(string graphicsApi, string config)
    {
        if (string.Equals(graphicsApi, "Vulkan", StringComparison.OrdinalIgnoreCase))
            return Path.Combine("Darwin", "DebugVulkan");
        var release = config.StartsWith("Release", StringComparison.OrdinalIgnoreCase);
        return Path.Combine("Darwin", release ? "Release" : "Debug");
    }

    /// <summary>
    /// Run 录制设置（Part B，Docs/2026-09-05）。录制本体在 PrimalPlayer 进程内
    /// （渲染帧与捕获帧 1:1，无丢帧）；编辑器只负责拼参数与落盘目录约定。
    /// </summary>
    public sealed record PlayerRecordOptions(bool Enabled, int Frames, int Every, bool Metrics);

    /// <summary>
    /// 拼装录制参数（纯函数，供单测）。语义与 PrimalPlayer main.cpp 对齐：
    /// --record 目录（frame_%04d.png）、--record-frames 捕获帧上限
    /// （0 = 跟随窗口关闭）、--record-every 步长、--record-metrics &lt;file&gt;
    /// （player 的行式解析器要求带显式路径——无值时 next() 返回 null，
    /// 该 token 会落入 unknown-arg 分支导致进程 exit 2）。
    /// </summary>
    public static void AppendRecordArguments(List<string> arguments, string recordDir, PlayerRecordOptions options)
    {
        arguments.AddRange(new[] { "--record", recordDir });
        arguments.AddRange(new[] { "--record-frames", Math.Max(0, options.Frames).ToString() });
        if (options.Every > 1)
            arguments.AddRange(new[] { "--record-every", options.Every.ToString() });
        if (options.Metrics)
            arguments.AddRange(new[] { "--record-metrics", Path.Combine(recordDir, "metrics.csv") });
    }

    /// <summary>录制产物目录约定：&lt;project&gt;/Captures/&lt;scene&gt;-&lt;yyyyMMdd-HHmmss&gt;。
    /// 场景名去文件系统非法字符（GetInvalidFileNameChars 只在 Windows 覆盖
    /// &lt; &gt; : 等，这里跨平台统一再补一组）；时间戳保证同场景多次录制不互覆盖。</summary>
    public static string ResolveRecordDirectory(string projectPath, string sceneName)
    {
        const string extraForbidden = "<>:\"|?*";
        var invalid = new char[Path.GetInvalidFileNameChars().Length + extraForbidden.Length];
        Path.GetInvalidFileNameChars().CopyTo(invalid, 0);
        extraForbidden.CopyTo(0, invalid, Path.GetInvalidFileNameChars().Length, extraForbidden.Length);
        var safe = string.Concat(sceneName.Select(c => invalid.Contains(c) ? '_' : c));
        return Path.Combine(projectPath, "Captures",
            $"{safe}-{DateTime.Now:yyyyMMdd-HHmmss}");
    }

    /// <summary>仓库根目录：编辑器二进制在
    /// &lt;root&gt;/Darwin/PrimalEditor_Avalonia/&lt;config&gt;/ 下，上溯三级；
    /// 打包/测试环境可用 PRIMAL_REPO_ROOT 覆盖。</summary>
    public static string? ResolveRepoRoot()
    {
        var env = Environment.GetEnvironmentVariable("PRIMAL_REPO_ROOT");
        if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;
        var asmDir = Path.GetDirectoryName(typeof(RuntimeLauncher).Assembly.Location);
        if (asmDir == null) return null;
        var dir = Path.GetFullPath(Path.Combine(asmDir, "..", "..", ".."));
        return Directory.Exists(Path.Combine(dir, "Darwin")) ? dir : null;
    }

    /// <summary>
    /// 拼装固定分辨率参数（纯函数，供单测）。语义与 PrimalPlayer main.cpp 对齐：
    /// --width/--height 指定离屏 RT 与窗口内容尺寸；--fixed 去掉 NSWindow 的
    /// Resizable 样式，保证显示分辨率与渲染分辨率严格一致。
    /// </summary>
    public static void AppendResolutionArguments(List<string> arguments, int width, int height)
    {
        arguments.AddRange(new[] { "--width", Math.Clamp(width, 16, 8192).ToString() });
        arguments.AddRange(new[] { "--height", Math.Clamp(height, 16, 8192).ToString() });
        arguments.Add("--fixed");
    }

    /// <summary>
    /// Export the live scene and launch PrimalPlayer. Returns false (with a
    /// logged reason) when there is no scene, the host binary is missing,
    /// or the process refused to start. RenderSurface mode never reaches the
    /// player — the caller (WorldEditorView / RunCommands) routes it to
    /// EngineViewport3D.BeginRuntimePreview instead.
    /// </summary>
    public static bool Run(ViewModels.WorldEditorViewModel vm, string config)
    {
        if (vm?.Project == null)
        {
            Logger.Warning("[Run] no project loaded — open a project first.");
            return false;
        }
        if (vm.ActiveScene == null)
        {
            Logger.Warning("[Run] no active scene to run.");
            return false;
        }
        if (ResolveMode(vm.RunModeIndex) == RunMode.RenderSurface)
        {
            Logger.Warning("[Run] RenderSurface 模式不开 PrimalPlayer —— 应走编辑器视口 runtime 预览。");
            return false;
        }

        var package = RuntimeSceneExporter.Export(vm.ActiveScene, vm.Project.Path);
        if (package == null)
        {
            Logger.Error("[Run] runtime scene export failed.");
            return false;
        }

        var root = ResolveRepoRoot();
        if (root == null)
        {
            Logger.Error("[Run] cannot locate repo root (set PRIMAL_REPO_ROOT).");
            return false;
        }

        var api = vm.TargetGraphicsApi;
        var playerDir = ResolvePlayerDir(api, config);
        var playerPath = Path.GetFullPath(Path.Combine(root, playerDir!, "PrimalPlayer"));
        if (!File.Exists(playerPath))
        {
            Logger.Error(
                $"[Run] PrimalPlayer not found: {playerPath} — build the '{api}/{config}' tree first " +
                $"(cmake --build <tree> --target PrimalPlayer).");
            return false;
        }

        var arguments = new List<string> { "--scene", package };

        // 临时绕行（2026-10-09）：PipelineSetEditorMode(0) 的 Metal runtime 全延迟
        // 路径当前输出全黑（编辑器视口/primal-engine 均走 mode 1 正常；证据见
        // Docs/completed/2026-10-09-runtime-chain-material-injection.md 的引擎侧
        // 移交段）。引擎侧修复后删除本段，恢复 Player 默认的 runtime 路径。
        arguments.Add("--editor-mode");
        arguments.Add("1");

        // 固定分辨率模式：窗口内容恰为 WxH 且不可缩放（确定性录制/比对前置）。
        if (ResolveMode(vm.RunModeIndex) == RunMode.FixedResolution)
            AppendResolutionArguments(arguments, vm.RunWidth, vm.RunHeight);

        // 录制目录只解析一次（时间戳目录——重复解析会得到不同路径）。
        var recordDir = vm.RecordOnRun
            ? ResolveRecordDirectory(vm.Project.Path, vm.ActiveScene.Name)
            : null;

        // Run 录制（Part B）：VM 的录制开关 → player 参数。录满帧数后 player
        // 自动退出，stderr 的 record summary 会经 BuildService.OnOutput 流入
        // BuildLog 面板（PNG 序列主产物；mp4 编码入口由 sequence 模块统一提供）。
        if (vm.RecordOnRun)
        {
            AppendRecordArguments(arguments, recordDir!, new PlayerRecordOptions(
                Enabled: true,
                Frames: vm.RecordFrames,
                Every: 1,
                Metrics: vm.RecordMetrics));
            Logger.Info($"[Run] recording enabled → {recordDir} (frames={vm.RecordFrames}, metrics={vm.RecordMetrics})");

            // 录完自动编码（2026-10-09 增补，09-05 二期 run.record 的编辑器侧
            // 最小版）：player 进程退出后把 PNG 序列合成 mp4——VideoEncoder 与
            // sequence.render 同一实现（libx264/crf20/yuv420p）。一段式订阅，
            // 触发即摘除；不因 player 退出码非零跳过（RHIAllocator 泄漏 abort
            // 期间帧序列仍完整，调试场景照样出片），仅记录退出码。
            // agent-workflows S1：编码登记为长任务（task.* 可查询，一次性
            // 任务不支持中途取消）。
            void EncodeOnExit(int exitCode, bool success)
            {
                BuildService.OnCompleted -= EncodeOnExit;
                if (!success)
                    Logger.Warning($"[Run] player exited with code {exitCode} — encoding captured frames anyway.");
                _ = System.Threading.Tasks.Task.Run(() =>
                {
                    var videoPath = Path.Combine(recordDir!, "capture.mp4");
                    var encodeTask = PrimalEditor_Avalonia.Cli.TaskRegistry.SubmitFromAnyThread(
                        "encode", $"PNG→mp4：{recordDir}",
                        artifacts: new[]
                        {
                            new PrimalEditor_Avalonia.Cli.TaskRegistry.TaskArtifact
                                { Path = videoPath, Kind = "video" },
                        });
                    var (encoded, hint) = VideoEncoder.EncodePngSequence(
                        recordDir, videoPath, vm.RunWidth, vm.RunHeight, framerate: 30);
                    PrimalEditor_Avalonia.Cli.TaskRegistry.Complete(
                        encodeTask, encoded, encoded ? null : hint);
                    if (encoded)
                        Logger.Info($"[Run] recording encoded → {videoPath}");
                    else
                        Logger.Warning($"[Run] auto-encode skipped: {hint} (PNG 序列保留在 {recordDir})");
                });
            }
            BuildService.OnCompleted += EncodeOnExit;
        }

        var ok = BuildService.StartPlayer(playerPath, arguments, workingDirectory: Path.GetDirectoryName(playerPath)!);
        if (ok)
        {
            Logger.Info($"[Run] PrimalPlayer launched (api={api}, config={config}, scene=\"{vm.ActiveScene.Name}\").");
            // agent-workflows S1：player 进程登记为可取消长任务（task.cancel =
            // BuildService.Stop 终止进程；录制产物在查询时实算存在性）。
            var playerTask = PrimalEditor_Avalonia.Cli.TaskRegistry.Submit(
                "player-run", $"PrimalPlayer：{vm.ActiveScene.Name}（api={api}）",
                cancel: BuildService.Stop,
                artifacts: vm.RecordOnRun
                    ? new[]
                    {
                        new PrimalEditor_Avalonia.Cli.TaskRegistry.TaskArtifact
                            { Path = recordDir!, Kind = "capture" },
                    }
                    : null);
            void PlayerTaskOnExit(int exitCode, bool success)
            {
                BuildService.OnCompleted -= PlayerTaskOnExit;
                PrimalEditor_Avalonia.Cli.TaskRegistry.Complete(
                    playerTask, success, success ? null : $"exit code {exitCode}");
            }
            BuildService.OnCompleted += PlayerTaskOnExit;
        }
        return ok;
    }
}
