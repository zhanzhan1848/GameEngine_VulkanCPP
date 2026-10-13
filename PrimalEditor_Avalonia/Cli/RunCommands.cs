using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Run 域命令：编辑器通道触发与工具栏 Run/Stop 按钮同一代码路径。
/// mode 三选一（§Run）：window=外部 PrimalPlayer 默认窗口（原路线）；
/// fixed=固定分辨率外部 PrimalPlayer（--width/--height/--fixed，确定性录制用）；
/// surface=不开外部进程，编辑器内嵌 render surface 切 runtime 渲染路径
/// （PipelineSetEditorMode(false)）+ 场景相机取景，record 参数转为编辑器侧
/// DispatcherTimer 逐帧捕获。run.stop 结束 surface 预览或停止 player 进程。
/// </summary>
public sealed class RunCommands
{
    [CliCommand("run.launch",
        Description = "按 mode 启动运行：" +
                      "window=导出运行时场景包并启动外部 PrimalPlayer（与工具栏 Run 按钮原路线相同）；" +
                      "fixed=同样外部 PrimalPlayer，但按 width/height 固定窗口分辨率且不可缩放" +
                      "（width/height 像素，16..8192，默认 1280x720）；" +
                      "surface=不开外部进程，编辑器内嵌渲染表面直接跑 runtime 渲染路径" +
                      "（deferred/RenderGraph，受 RENDER 组合框 pass 开关控制），透过场景 Camera 实体取景，" +
                      "场景无 Camera 实体返回 ENGINE_ERROR；再次 run.launch 幂等。" +
                      "record=true 时录制逐帧 PNG（frame_%04d.png）：window/fixed 由 player 进程内捕获" +
                      "（渲染帧与捕获帧 1:1，录满 recordFrames 自动退出，0=跟随窗口关闭）；" +
                      "surface 由编辑器侧逐帧捕获（PNG 编码含 GPU WaitIdle，捕获率低于帧率，立即返回）。" +
                      "recordMetrics=true 同时写逐帧亮度 CSV。产物目录 <project>/Captures/<scene>-<时间戳>/。" +
                      "无项目/无激活场景返回 NOT_READY；player 二进制缺失返回 ENGINE_ERROR。" +
                      "window/fixed 模式本命令立即返回启动结果，不等待 player 退出" +
                      "（用 pgrep 或 Captures 目录判断录制完成）；surface 模式用 run.stop 结束。",
        Example = """{"mode":"surface","record":true,"recordFrames":60,"recordMetrics":true}""")]
    public object Launch(string mode = "window", int width = 1280, int height = 720,
        bool record = false, int recordFrames = 300, bool recordMetrics = true)
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        if (worldEditor.ActiveScene == null)
            throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");

        if (string.Equals(mode, "surface", StringComparison.OrdinalIgnoreCase))
            return LaunchInSurface(worldEditor, record, recordFrames, recordMetrics);
        if (!string.Equals(mode, "window", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(mode, "fixed", StringComparison.OrdinalIgnoreCase))
            throw CliError.InvalidParams($"未知 mode '{mode}'",
                "有效取值: window / fixed / surface");

        // window/fixed：经 VM 传递（RuntimeLauncher.Run 的唯一参数源），启动后恢复原值。
        var oldMode = worldEditor.RunModeIndex;
        var oldWidth = worldEditor.RunWidth;
        var oldHeight = worldEditor.RunHeight;
        var oldRecord = worldEditor.RecordOnRun;
        var oldFrames = worldEditor.RecordFrames;
        var oldMetrics = worldEditor.RecordMetrics;
        worldEditor.RunModeIndex = string.Equals(mode, "fixed", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        worldEditor.RunWidth = Math.Clamp(width, 16, 8192);
        worldEditor.RunHeight = Math.Clamp(height, 16, 8192);
        worldEditor.RecordOnRun = record;
        worldEditor.RecordFrames = Math.Clamp(recordFrames, 0, 100_000);
        worldEditor.RecordMetrics = recordMetrics;
        try
        {
            var config = worldEditor.Project?.BuildConfigName ?? "Debug";
            var ok = RuntimeLauncher.Run(worldEditor, config);
            if (!ok)
                throw new CliError(CliErrorCodes.EngineError,
                    "PrimalPlayer 启动失败（二进制缺失或导出失败）",
                    $"先构建目标树: cmake --build Darwin/{(string.Equals(worldEditor.TargetGraphicsApi, "Vulkan", StringComparison.OrdinalIgnoreCase) ? "DebugVulkan" : "Debug")} --target PrimalPlayer");
            var recordDir = record
                ? RuntimeLauncher.ResolveRecordDirectory(worldEditor.Project!.Path, worldEditor.ActiveScene.Name)
                : null;
            return new
            {
                launched = true,
                mode = mode.ToLowerInvariant(),
                api = worldEditor.TargetGraphicsApi,
                config,
                scene = worldEditor.ActiveScene.Name,
                resolution = string.Equals(mode, "fixed", StringComparison.OrdinalIgnoreCase)
                    ? new[] { worldEditor.RunWidth, worldEditor.RunHeight }
                    : null,
                record,
                recordDir,
                recordFrames = record ? worldEditor.RecordFrames : 0,
            };
        }
        finally
        {
            worldEditor.RunModeIndex = oldMode;
            worldEditor.RunWidth = oldWidth;
            worldEditor.RunHeight = oldHeight;
            worldEditor.RecordOnRun = oldRecord;
            worldEditor.RecordFrames = oldFrames;
            worldEditor.RecordMetrics = oldMetrics;
        }
    }

    /// <summary>surface 模式：编辑器内嵌 render surface 的 runtime 预览。</summary>
    private static object LaunchInSurface(ViewModels.WorldEditorViewModel worldEditor,
        bool record, int recordFrames, bool recordMetrics)
    {
        var viewport = EditorServices.FindViewport()
            ?? throw CliError.NotReady("3D 视口尚未创建", "打开项目并等 WorldEditor 视图就绪后重试");
        if (!viewport.BeginRuntimePreview(worldEditor))
            throw new CliError(CliErrorCodes.EngineError,
                "render-surface runtime 预览启动失败",
                "场景需要至少一个 Camera 实体且已同步到引擎（entity.create kind=Camera）");

        string? recordDir = null;
        var frames = Math.Clamp(recordFrames, 0, 100_000);
        if (record && worldEditor.Project != null && frames > 0)
        {
            recordDir = RuntimeLauncher.ResolveRecordDirectory(worldEditor.Project.Path, worldEditor.ActiveScene!.Name);
            viewport.StartPreviewFrameCapture(recordDir, frames);
        }
        return new
        {
            launched = true,
            mode = "surface",
            api = "editor viewport（与编辑器同进程同管线）",
            scene = worldEditor.ActiveScene!.Name,
            camera = "场景 Camera 实体（PipelineSetEditorMode(false) runtime 路径）",
            record,
            recordDir,
            recordFrames = record ? frames : 0,
            knownLimitation = "编辑器同步链尚未接入 runtime 实体注册（RenderSceneSnapshot），surface 预览画面暂不含场景实体（引擎侧集成项）",
            hint = "editor.screenshot / editor.record 在预览中即捕获 runtime 画面；run.stop 结束预览",
        };
    }

    [CliCommand("run.export",
        Description = "导出运行时场景包（与 Run 按钮同一生成器，不启动进程）。" +
                      "portable=true 时把引用的 cooked mesh/纹理/脚本复制进包目录 _assets/，" +
                      "行内路径改写为包相对（_assets/…）——整个 portable 目录可 zip 分发，" +
                      "Player 解析顺序=包内相对→项目根，换机解压即跑（release S1 可搬移资源包）。" +
                      "返回包路径与资产清单（files=包相对路径数组，bytes=复制总字节；非 portable 为空）。" +
                      "无项目/无激活场景返回 NOT_READY。",
        Example = """{"portable":true}""")]
    public object ExportScene(bool portable = false)
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        if (worldEditor.ActiveScene == null)
            throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");
        var projectDir = worldEditor.Project?.Path
            ?? throw CliError.NotReady("项目未加载", "先在编辑器中打开项目");

        var path = PrimalEditor_Avalonia.Content.RuntimeSceneExporter.Export(
            worldEditor.ActiveScene, projectDir, portable)
            ?? throw new CliError(CliErrorCodes.EngineError, "运行时场景包导出失败", "查看编辑器日志");

        string[] files = Array.Empty<string>();
        long bytes = 0;
        if (portable)
        {
            var assetsDir = System.IO.Path.Combine(
                System.IO.Path.GetDirectoryName(path)!,
                PrimalEditor_Avalonia.Content.RuntimeSceneExporter.PortableAssetsDirName);
            if (System.IO.Directory.Exists(assetsDir))
            {
                files = System.IO.Directory.GetFiles(assetsDir)
                    .Select(f => $"{PrimalEditor_Avalonia.Content.RuntimeSceneExporter.PortableAssetsDirName}/{System.IO.Path.GetFileName(f)}")
                    .OrderBy(f => f, StringComparer.Ordinal)
                    .ToArray();
                bytes = files.Sum(f => new System.IO.FileInfo(System.IO.Path.Combine(assetsDir, System.IO.Path.GetFileName(f))).Length);
            }
        }
        return new { path, portable, files, bytes };
    }

    [CliCommand("run.stop",
        Description = "结束运行：surface 模式的 runtime 预览先于外部进程——预览激活时退出预览并恢复" +
                      "编辑器渲染路径（PipelineSetEditorMode(true) + 编辑器相机 + 叠层）；" +
                      "否则停止 window/fixed 模式启动的 PrimalPlayer 进程（若有）。幂等：两者都不活跃时" +
                      "返回 stopped=false 而非错误。",
        Example = "{}")]
    public object Stop()
    {
        if (EditorServices.FindViewport() is { IsRuntimePreview: true } viewport)
        {
            viewport.EndRuntimePreview();
            return new { stopped = true, what = "render-surface runtime preview" };
        }
        var hadProcess = BuildService.IsRunning;
        BuildService.Stop();
        return new { stopped = hadProcess, what = hadProcess ? "PrimalPlayer process" : (string?)"nothing running" };
    }
}
