using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
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
    private static readonly SemaphoreSlim BackupGate = new(1, 1);

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
        var projectChanged = !ReferenceEquals(_trackedProject, project);
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
        if (projectChanged)
        {
            _lastDirtyAt = project?.IsDirty == true ? DateTime.UtcNow : DateTime.MinValue;
            _lastBackupAt = DateTime.MinValue;
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
        if (!await BackupGate.WaitAsync(0)) return;

        string? stagingRoot = null;

        try
        {
            var project = _trackedProject;
            var trackedPath = _trackedPath;
            if (project == null || trackedPath == null) return;

            _lastBackupAt = DateTime.UtcNow;

            // Capture detached documents before entering the worker thread.
            // The UI can continue mutating the live Project/Scene objects while
            // the detached snapshot is written, and no dirty flags are touched.
            var projectDocument = Serializer.ToDocument(project);
            var sceneDocuments = project.Scenes
                .Select(Serializer.ToSceneDocument)
                .ToArray();

            var projectDir = Path.GetDirectoryName(trackedPath)
                ?? throw new InvalidOperationException("project has no directory");
            var projectName = string.IsNullOrEmpty(project.Name) ? "project" : project.Name;
            var bundleName = $"{projectName}_{DateTime.UtcNow:yyyyMMdd_HHmmssfff}";
            var autosaveRoot = Path.Combine(projectDir, ".autosave");
            var finalRoot = Path.Combine(autosaveRoot, bundleName);
            stagingRoot = Path.Combine(autosaveRoot, $".partial-{Guid.NewGuid():N}");
            var manifestName = Path.GetFileName(trackedPath);

            await Task.Run(() =>
            {
                Directory.CreateDirectory(autosaveRoot);
                Directory.CreateDirectory(stagingRoot);
                var stagingScenes = Path.Combine(stagingRoot, "Scenes");
                Directory.CreateDirectory(stagingScenes);

                var seenSceneGuids = new HashSet<GameProject.Identifiers.SceneGuid>();
                foreach (var sceneDocument in sceneDocuments)
                {
                    if (!seenSceneGuids.Add(sceneDocument.SceneGuid))
                        throw new InvalidDataException($"Duplicate SceneGuid in autosave snapshot: {sceneDocument.SceneGuid}");

                    var scenePath = Path.Combine(stagingScenes,
                        sceneDocument.SceneGuid.Value.ToString("D").ToLowerInvariant() + Scene.Extension);
                    Serializer.SaveSceneDocument(sceneDocument, scenePath);
                    var loadedScene = Serializer.LoadScene(scenePath);
                    if (loadedScene?.Guid != sceneDocument.SceneGuid)
                        throw new InvalidDataException($"Autosave scene validation failed: {scenePath}");
                }

                var manifestPath = Path.Combine(stagingRoot, manifestName);
                Serializer.SaveProjectDocument(projectDocument, manifestPath);
                var loadedProject = Serializer.LoadProject(manifestPath)
                    ?? throw new InvalidDataException("Autosave project validation failed.");
                if (!loadedProject.Scenes.Select(scene => scene.Guid).SequenceEqual(
                        projectDocument.Scenes))
                    throw new InvalidDataException("Autosave project scene list validation failed.");

                if (Directory.Exists(finalRoot))
                    finalRoot = Path.Combine(autosaveRoot, $"{bundleName}_{Guid.NewGuid():N}");
                Directory.Move(stagingRoot, finalRoot);
            });

            stagingRoot = null;
            Logger.Info($"AutoSave: {finalRoot}");
            CleanupOldBackups(autosaveRoot, projectName, keep: 5);
        }
        catch (Exception ex)
        {
            Logger.Error($"AutoSave failed: {ex.Message}");
        }
        finally
        {
            if (!string.IsNullOrEmpty(stagingRoot))
            {
                try { if (Directory.Exists(stagingRoot)) Directory.Delete(stagingRoot, recursive: true); } catch { }
            }
            BackupGate.Release();
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
