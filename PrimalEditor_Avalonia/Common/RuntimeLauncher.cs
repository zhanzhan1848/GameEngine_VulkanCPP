using System;
using System.Collections.Generic;
using System.IO;
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
    /// <summary>(graphicsApi, buildConfig) → 构建树目录（repo 相对）。
    /// Vulkan 树目前只有 Debug 配置产物（Darwin/DebugVulkan）。</summary>
    public static string? ResolvePlayerDir(string graphicsApi, string config)
    {
        if (string.Equals(graphicsApi, "Vulkan", StringComparison.OrdinalIgnoreCase))
            return Path.Combine("Darwin", "DebugVulkan");
        var release = config.StartsWith("Release", StringComparison.OrdinalIgnoreCase);
        return Path.Combine("Darwin", release ? "Release" : "Debug");
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
    /// Export the live scene and launch PrimalPlayer. Returns false (with a
    /// logged reason) when there is no scene, the host binary is missing,
    /// or the process refused to start.
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
        var ok = BuildService.StartPlayer(playerPath, arguments, workingDirectory: Path.GetDirectoryName(playerPath)!);
        if (ok)
        {
            Logger.Info($"[Run] PrimalPlayer launched (api={api}, config={config}, scene=\"{vm.ActiveScene.Name}\").");
        }
        return ok;
    }
}
