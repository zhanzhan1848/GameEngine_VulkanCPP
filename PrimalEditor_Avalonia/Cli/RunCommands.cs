using System;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Run 域命令（Part B 路线图 `run.record` 的一期版）：编辑器通道触发
/// RuntimeLauncher.Run（与 Run 按钮同一代码路径），支持附带 PrimalPlayer
/// 逐帧录制参数。录满帧数后 player 自动退出，"进程退出 = 录制完成"；
/// 产物（PNG 序列 + 可选 CSV 遥测）在返回的 recordDir 下。
/// </summary>
public sealed class RunCommands
{
    [CliCommand("run.launch",
        Description = "导出运行时场景包并启动 PrimalPlayer 外部宿主（与工具栏 Run 按钮同一 RuntimeLauncher 路径）。" +
                      "record=true 时附带逐帧录制：渲染帧与捕获帧 1:1 落 PNG（frame_%04d.png），录满 " +
                      "recordFrames 帧后 player 自动退出（0 = 跟随窗口关闭）；recordMetrics=true 同时写逐帧亮度 " +
                      "CSV（口径同 editor.metrics）。产物目录 <project>/Captures/<scene>-<时间戳>/。" +
                      "无项目/无激活场景返回 NOT_READY；player 二进制缺失返回 ENGINE_ERROR。本命令立即返回启动结果，" +
                      "不等待 player 退出（用 pgrep 或 Captures 目录判断录制完成）。",
        Example = """{"record":true,"recordFrames":60,"recordMetrics":true}""")]
    public object Launch(bool record = false, int recordFrames = 300, bool recordMetrics = true)
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        if (worldEditor.ActiveScene == null)
            throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");

        // 录制参数经 VM 传递（RuntimeLauncher.Run 的唯一参数源），启动后恢复原值。
        var oldRecord = worldEditor.RecordOnRun;
        var oldFrames = worldEditor.RecordFrames;
        var oldMetrics = worldEditor.RecordMetrics;
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
                api = worldEditor.TargetGraphicsApi,
                config,
                scene = worldEditor.ActiveScene.Name,
                record,
                recordDir,
                recordFrames = record ? worldEditor.RecordFrames : 0,
            };
        }
        finally
        {
            worldEditor.RecordOnRun = oldRecord;
            worldEditor.RecordFrames = oldFrames;
            worldEditor.RecordMetrics = oldMetrics;
        }
    }
}
