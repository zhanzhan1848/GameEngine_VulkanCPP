using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject.Migration;

namespace PrimalEditor_Avalonia.GameProject;

public static class OpenProject
{
    public static Project? Open(string filePath)
    {
        if (!File.Exists(filePath))
        {
            Logger.Error($"Project file not found: {filePath}");
            return null;
        }

        var migration = new ProjectMigrationService().MigrateIfNeeded(filePath);
        if (!migration.Success)
        {
            Logger.Error($"Project migration failed: {migration.ErrorMessage}");
            return null;
        }

        var project = Serializer.LoadProject(filePath);
        if (project == null)
        {
            return null;
        }

        // 引擎日志文件落地（CLI 规划 §11.2 L2 FileSink）：<项目>/.Primal/logs/engine.log，
        // 8MiB × 3 轮转。旧 dylib 无导出时静默跳过。
        try
        {
            var engineLogDir = Path.Combine(Path.GetDirectoryName(filePath) ?? ".", ".Primal", "logs");
            Directory.CreateDirectory(engineLogDir);
            PrimalEditor_Avalonia.Native.NativeEngineBridge.SetLogFile(
                Path.Combine(engineLogDir, "engine.log"), 8 * 1024 * 1024, 3);
        }
        catch { /* 日志文件不可用不影响项目打开 */ }

        if (!string.IsNullOrEmpty(project.Path))
        {
            Directory.CreateDirectory(project.ContentPath);
            Directory.CreateDirectory(project.ScenesPath);
            Directory.CreateDirectory(project.TempFolder);
        }

        // Load every scene referenced by the manifest so the
        // project doesn't show empty placeholders. Failures are
        // logged per-scene and the project still opens.
        Serializer.LoadProjectScenes(project);

        Logger.Info($"Project opened: {project.FullPath} ({project.Scenes.Count} scene(s))");
        return project;
    }
}