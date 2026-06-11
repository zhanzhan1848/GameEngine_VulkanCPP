using System;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

public class AutoSaveService
{
    private static DispatcherTimer? _timer;
    private static Project? _trackedProject;
    private static string? _trackedPath;
    private static DateTime _lastChange = DateTime.MinValue;

    public static TimeSpan Interval { get; set; } = TimeSpan.FromMinutes(5);
    public static TimeSpan DebounceDelay { get; set; } = TimeSpan.FromSeconds(10);

    public static void Start()
    {
        if (_timer != null) return;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
        _timer.Tick += OnTick;
        _timer.Start();
        Logger.Info("AutoSave started (interval=5min, debounce=10s)");
    }

    public static void Stop()
    {
        _timer?.Stop();
        _timer = null;
    }

    public static void Track(Project? project)
    {
        _trackedProject = project;
        _trackedPath = project?.FullPath;
        _lastChange = DateTime.UtcNow;
    }

    public static void NotifyChanged()
    {
        _lastChange = DateTime.UtcNow;
    }

    private static void OnTick(object? sender, EventArgs e)
    {
        if (_trackedProject == null || _trackedPath == null) return;

        if (DateTime.UtcNow - _lastChange < DebounceDelay) return;

        if (File.Exists(_trackedPath))
        {
            var lastWrite = File.GetLastWriteTimeUtc(_trackedPath);
            if (DateTime.UtcNow - lastWrite < Interval) return;
        }

        _ = SaveBackupAsync();
    }

    public static async Task SaveBackupAsync()
    {
        if (_trackedProject == null) return;
        try
        {
            var configDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia", "autosave");
            Directory.CreateDirectory(configDir);

            var projectName = string.IsNullOrEmpty(_trackedProject.Name) ? "project" : _trackedProject.Name;
            var backupPath = Path.Combine(configDir, $"{projectName}_{DateTime.UtcNow:yyyyMMdd_HHmmss}.primal");

            await Task.Run(() => Serializer.SaveProject(_trackedProject, backupPath));
            Logger.Info($"AutoSave: {backupPath}");

            CleanupOldBackups(configDir, projectName, keep: 10);
        }
        catch (Exception ex)
        {
            Logger.Warning($"AutoSave failed: {ex.Message}");
        }
    }

    private static void CleanupOldBackups(string dir, string projectName, int keep)
    {
        try
        {
            var files = Directory.GetFiles(dir, $"{projectName}_*.primal");
            if (files.Length <= keep) return;
            Array.Sort(files);
            for (int i = 0; i < files.Length - keep; i++)
            {
                try { File.Delete(files[i]); } catch { }
            }
        }
        catch { }
    }
}