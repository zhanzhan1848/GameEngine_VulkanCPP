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
    private static DateTime _lastDirtyAt = DateTime.MinValue;
    private static DateTime _lastBackupAt = DateTime.MinValue;

    public static TimeSpan Interval { get; set; } = TimeSpan.FromMinutes(5);
    public static TimeSpan DebounceDelay { get; set; } = TimeSpan.FromSeconds(10);

    public static void Start()
    {
        if (_timer != null) return;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _timer.Tick += OnTick;
        _timer.Start();
        Logger.Info($"AutoSave started (interval={Interval.TotalSeconds}s, debounce={DebounceDelay.TotalSeconds}s)");
    }

    public static void Stop()
    {
        _timer?.Stop();
        _timer = null;
    }

    public static void Track(Project? project)
    {
        if (_trackedProject != null)
        {
            _trackedProject.PropertyChanged -= OnTrackedProjectPropertyChanged;
        }
        _trackedProject = project;
        _trackedPath = project?.FullPath;
        if (project != null)
        {
            project.PropertyChanged += OnTrackedProjectPropertyChanged;
            if (project.IsDirty) _lastDirtyAt = DateTime.UtcNow;
        }
    }

    private static void OnTrackedProjectPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(Project.IsDirty) && _trackedProject?.IsDirty == true)
        {
            _lastDirtyAt = DateTime.UtcNow;
        }
    }

    public static void NotifyChanged()
    {
        _lastDirtyAt = DateTime.UtcNow;
    }

    private static void OnTick(object? sender, EventArgs e)
    {
        if (_trackedProject == null || _trackedPath == null) return;
        if (!_trackedProject.IsDirty) return;
        if (DateTime.UtcNow - _lastDirtyAt < DebounceDelay) return;
        if (DateTime.UtcNow - _lastBackupAt < Interval) return;

        _ = SaveBackupAsync();
    }

    public static async Task SaveBackupAsync()
    {
        if (_trackedProject == null || _trackedPath == null) return;
        _lastBackupAt = DateTime.UtcNow;

        try
        {
            var projectDir = Path.GetDirectoryName(_trackedPath)
                ?? throw new InvalidOperationException("project has no directory");
            var projectName = string.IsNullOrEmpty(_trackedProject.Name) ? "project" : _trackedProject.Name;
            var bundleName = $"{projectName}_{DateTime.UtcNow:yyyyMMdd_HHmmssfff}";
            var backupRoot = Path.Combine(projectDir, ".autosave", bundleName);
            Directory.CreateDirectory(backupRoot);
            Directory.CreateDirectory(Path.Combine(backupRoot, "Scenes"));

            // Copy each scene's source file into the bundle so the snapshot is
            // self-contained. Serializer.SaveProject writes scenes under
            // project.ScenesPath (the live project directory), not next to
            // the backup .primal, so without this copy the bundle has a
            // .primal referencing scenes that won't move with it.
            var liveScenesDir = Path.Combine(projectDir, "Scenes");
            foreach (var scene in _trackedProject.Scenes)
            {
                var rel = $"{scene.Guid.Value.ToString("D").ToLowerInvariant()}{Scene.Extension}";
                var src = Path.Combine(liveScenesDir, rel);
                var dst = Path.Combine(backupRoot, "Scenes", rel);
                if (File.Exists(src)) File.Copy(src, dst, overwrite: true);
            }

            await Task.Run(() => Serializer.SaveProject(_trackedProject, Path.Combine(backupRoot, Path.GetFileName(_trackedPath))));
            Logger.Info($"AutoSave: {backupRoot}");

            CleanupOldBackups(Path.Combine(projectDir, ".autosave"), projectName, keep: 5);
        }
        catch (Exception ex)
        {
            Logger.Error($"AutoSave failed: {ex.Message}");
        }
    }

    private static void CleanupOldBackups(string dir, string projectName, int keep)
    {
        try
        {
            var prefix = string.IsNullOrEmpty(projectName) ? "project" : projectName;
            var bundles = Directory.GetDirectories(dir, $"{prefix}_*");
            if (bundles.Length <= keep) return;
            Array.Sort(bundles);
            for (int i = 0; i < bundles.Length - keep; i++)
            {
                try { Directory.Delete(bundles[i], recursive: true); } catch { }
            }
        }
        catch { }
    }
}