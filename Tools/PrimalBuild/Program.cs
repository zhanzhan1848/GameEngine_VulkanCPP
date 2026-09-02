using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;

// =============================================================================
// primal-build — 场景/项目离线打包工具（CLI 规划 §10 Phase 3 场景烘焙断链·路线 A）
//
//   dotnet run --project Tools/PrimalBuild -- <project.primal> [--save-only]
//
// 无 GUI 进程直接复用编辑器程序集的打包链（零逻辑搬迁、零漂移）：
//   OpenProject.Open（含迁移） → BuildPipeline.ExecuteRebuild
//   （AssetRegistry 重扫 → BuildManifestGenerator → 落盘 manifest）
//   → EditorSaveService.SaveAll（场景 + 项目清单落盘）
//
// 输出契约与 primal 全家一致：stdout=JSON、stderr=日志；退出码 0 成功 / 2 参数错 / 4 内部错。
// --save-only 跳过资产重打包，只做场景/清单落盘（轻量校验路径）。
// =============================================================================

return PrimalBuild.Main(args);

internal static class PrimalBuild
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static int Main(string[] args)
    {
        try
        {
            return Run(args);
        }
        catch (Exception ex)
        {
            Console.Out.WriteLine(JsonSerializer.Serialize(new
            {
                ok = false,
                error = new { code = "INTERNAL", message = ex.Message },
            }, JsonOpts));
            return 4;
        }
    }

    private static int Run(string[] args)
    {
        string? projectFile = null;
        var saveOnly = false;
        foreach (var arg in args)
        {
            if (arg == "--save-only") saveOnly = true;
            else if (arg.EndsWith(".primal", StringComparison.OrdinalIgnoreCase)) projectFile = arg;
            else if (arg is "-h" or "--help")
            {
                Console.Error.WriteLine("""
                    primal-build — 场景/项目离线打包（断链路线 A）

                    用法:
                      primal-build <project.primal> [--save-only]

                    stdout=JSON（打包摘要），stderr=日志；退出码 0/2/4
                    """);
                return 2;
            }
            else
            {
                Fail(2, $"无法识别的参数 '{arg}'", "用法: primal-build <project.primal> [--save-only]");
                return 2;
            }
        }

        if (projectFile == null)
        {
            Fail(2, "缺少 <project.primal> 参数", "示例: primal-build projects_test/TestProject_01/TestProject_01.primal");
            return 2;
        }
        if (!File.Exists(projectFile))
        {
            Fail(2, $"项目文件不存在: {projectFile}", "确认相对路径基于当前工作目录");
            return 2;
        }

        var project = OpenProject.Open(projectFile);
        if (project == null)
        {
            Fail(4, $"项目打开失败（迁移/清单校验）：{projectFile}", "查看 stderr 日志定位");
            return 4;
        }

        // manifest 类型由 BuildPipeline 返回值推断
        var assets = 0;
        if (!saveOnly)
        {
            var manifest = BuildPipeline.ExecuteRebuild(project);
            assets = manifest.Entries.Count;
        }

        var saved = EditorSaveService.Instance.SaveAll(project);
        if (!saved)
        {
            Fail(4, "场景/清单落盘失败：" + string.Join("; ", EditorSaveService.LastSaveAllFailures),
                "检查磁盘空间与项目目录写权限");
            return 4;
        }

        Console.Out.WriteLine(JsonSerializer.Serialize(new
        {
            ok = true,
            data = new
            {
                project = project.Name,
                projectFile = Path.GetFullPath(projectFile),
                scenes = project.Scenes.Select(s => new { name = s.Name, guid = s.Guid.Value, entities = s.Entities.Count }),
                repackaged = !saveOnly,
                assets,
                manifestPath = Path.GetFullPath(Path.Combine(project.Path, ".Primal", "build")),
            },
        }, JsonOpts));
        return 0;
    }

    private static void Fail(int code, string message, string hint)
    {
        Console.Error.WriteLine($"primal-build: {message}");
        Console.Error.WriteLine($"  hint: {hint}");
        Console.Out.WriteLine(JsonSerializer.Serialize(new
        {
            ok = false,
            error = new { code = code == 2 ? "INVALID_PARAMS" : "INTERNAL", message, hint },
        }, JsonOpts));
    }
}
