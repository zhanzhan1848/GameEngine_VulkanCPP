using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Outcome of asking the user to recover a single autosave bundle.
/// </summary>
public enum AutoSaveRecoveryDecision
{
    RecoverAutosave,
    OpenSavedVersion,
    Later,
}

/// <summary>
/// Pure (UI-independent) autosave recovery helpers. Discovery, validation,
/// freshness comparison, dismissal and recovered-project loading all live
/// here so they can be exercised without a real window or dispatcher.
/// </summary>
public static class AutoSaveRecoveryService
{
    public const string DismissalMarkerName = ".dismissed";

    internal const string TimestampFormat = "yyyyMMdd_HHmmssfff";

    private static readonly Regex BundleNameRegex = new(
        @"^(?<project>.+?)_(?<date>\d{8})_(?<time>\d{9})(?:_(?<guid>[0-9a-fA-F]{32}))?$",
        RegexOptions.Compiled);

    public sealed class AutoSaveBundle
    {
        public string BundlePath { get; init; } = string.Empty;
        public string ProjectName { get; init; } = string.Empty;
        public DateTime Timestamp { get; init; }
        public string? ManifestFilePath { get; init; }
    }

    /// <summary>
    /// Enumerate direct, completed autosave bundles under
    /// <paramref name="projectDir"/>/.autosave. Hidden
    /// <c>.partial-*</c> staging directories are skipped. Each
    /// surviving candidate has a parseable manifest, every referenced
    /// scene reloads through the production serializers, and is not
    /// marked dismissed.
    /// </summary>
    public static IReadOnlyList<AutoSaveBundle> DiscoverBundles(string projectDir)
    {
        if (string.IsNullOrEmpty(projectDir)) return Array.Empty<AutoSaveBundle>();
        var autosaveDir = Path.Combine(projectDir, ".autosave");
        if (!Directory.Exists(autosaveDir)) return Array.Empty<AutoSaveBundle>();

        string[] bundleDirs;
        try
        {
            bundleDirs = Directory.GetDirectories(autosaveDir);
        }
        catch (Exception ex)
        {
            Logger.Warning($"AutoSave recovery: cannot enumerate {autosaveDir}: {ex.Message}");
            return Array.Empty<AutoSaveBundle>();
        }

        var candidates = new List<AutoSaveBundle>();
        foreach (var dir in bundleDirs)
        {
            var name = Path.GetFileName(dir);
            if (string.IsNullOrEmpty(name)) continue;
            if (name.StartsWith(".", StringComparison.Ordinal)) continue;
            if (!TryParseBundleName(name, out var projectName, out var timestamp)) continue;

            string manifestPath;
            try
            {
                manifestPath = Path.Combine(dir, projectName + Project.Extension);
            }
            catch
            {
                continue;
            }

            if (!File.Exists(manifestPath))
            {
                try
                {
                    var found = Directory.GetFiles(dir, "*" + Project.Extension).FirstOrDefault();
                    if (found == null) continue;
                    manifestPath = found;
                }
                catch
                {
                    continue;
                }
            }

            if (!ValidateBundle(dir, manifestPath)) continue;
            if (IsDismissed(dir)) continue;

            candidates.Add(new AutoSaveBundle
            {
                BundlePath = dir,
                ProjectName = projectName,
                Timestamp = timestamp,
                ManifestFilePath = manifestPath,
            });
        }

        return candidates
            .OrderByDescending(b => b.Timestamp)
            .ThenByDescending(b => b.BundlePath, StringComparer.Ordinal)
            .ToList();
    }

    /// <summary>
    /// Pick the newest valid bundle that is eligible to recover
    /// against <paramref name="projectFilePath"/>. A bundle is
    /// eligible when its timestamp is newer than the maximum UTC
    /// mtime of every referenced live file (manifest + each
    /// referenced scene), or when the live project cannot be
    /// loaded at all (missing or invalid manifest, or any
    /// referenced scene missing/corrupt).
    /// Results are deterministic: the input list is re-sorted by
    /// timestamp descending with a stable path tie-breaker, bundles
    /// for project names other than the target are ignored, and
    /// dismissed bundles are skipped.
    /// </summary>
    public static AutoSaveBundle? SelectNewestEligible(
        string projectFilePath,
        IReadOnlyList<AutoSaveBundle>? bundles,
        IEnumerable<AutoSaveBundle>? additionalDismissed = null)
    {
        if (bundles == null || bundles.Count == 0) return null;

        var targetProjectName = string.IsNullOrEmpty(projectFilePath)
            ? string.Empty
            : Path.GetFileNameWithoutExtension(projectFilePath);

        var dismissedSet = new HashSet<string>(StringComparer.Ordinal);
        if (additionalDismissed != null)
        {
            foreach (var dismissed in additionalDismissed)
            {
                if (dismissed != null) dismissedSet.Add(dismissed.BundlePath);
            }
        }

        var sorted = bundles
            .Where(b => b != null)
            .Where(b => !dismissedSet.Contains(b.BundlePath))
            .Where(b => !IsDismissed(b.BundlePath))
            .Where(b => string.IsNullOrEmpty(targetProjectName)
                        || string.Equals(b.ProjectName, targetProjectName, StringComparison.Ordinal))
            .OrderByDescending(b => b.Timestamp)
            .ThenByDescending(b => b.BundlePath, StringComparer.Ordinal)
            .ToList();

        if (sorted.Count == 0) return null;

        var baseline = TryGetLiveBaselineMaxTimestampUtc(projectFilePath);
        if (!baseline.HasValue)
        {
            return sorted[0];
        }

        var stamp = baseline.Value;
        foreach (var bundle in sorted)
        {
            if (bundle.Timestamp <= stamp) continue;
            return bundle;
        }
        return null;
    }

    /// <summary>
    /// Load a bundle into memory and rebase its <c>Name</c> and
    /// <c>Path</c> to <paramref name="originalProjectFilePath"/>.
    /// This routine performs no writes — it returns the rebased
    /// <see cref="Project"/> for the caller to assign. Returns null
    /// if the bundle cannot be loaded, rebound, or any referenced
    /// scene fails to load atomically.
    /// </summary>
    public static Project? LoadBundle(AutoSaveBundle bundle, string originalProjectFilePath)
    {
        if (bundle == null) throw new ArgumentNullException(nameof(bundle));
        if (string.IsNullOrEmpty(originalProjectFilePath))
            throw new ArgumentException("originalProjectFilePath required", nameof(originalProjectFilePath));

        var manifestPath = bundle.ManifestFilePath
            ?? Path.Combine(bundle.BundlePath, bundle.ProjectName + Project.Extension);
        if (!File.Exists(manifestPath)) return null;

        Project? loaded;
        try
        {
            loaded = Serializer.LoadProject(manifestPath);
        }
        catch (Exception ex)
        {
            Logger.Error($"AutoSave recovery: failed to load manifest {manifestPath}", ex);
            return null;
        }
        if (loaded == null) return null;

        var restoredScenes = new List<Scene>();
        foreach (var scene in loaded.Scenes)
        {
            var scenePath = Path.Combine(bundle.BundlePath, "Scenes",
                scene.Guid.Value.ToString("D").ToLowerInvariant() + Scene.Extension);

            if (!File.Exists(scenePath))
            {
                Logger.Error($"AutoSave recovery: scene payload missing for '{scene.Name}' at {scenePath}");
                return null;
            }

            Scene? restored;
            try
            {
                restored = Serializer.LoadScene(scenePath);
            }
            catch (Exception ex)
            {
                Logger.Error($"AutoSave recovery: failed to load scene {scenePath}", ex);
                return null;
            }
            if (restored == null)
            {
                Logger.Error($"AutoSave recovery: scene payload unreadable at {scenePath}");
                return null;
            }
            restoredScenes.Add(restored);
        }

        for (var i = 0; i < loaded.Scenes.Count && i < restoredScenes.Count; i++)
        {
            var scene = loaded.Scenes[i];
            var restored = restoredScenes[i];
            scene.Name = restored.Name;
            scene.IsActive = restored.IsActive;
            scene.Entities.Clear();
            foreach (var entity in restored.Entities)
            {
                RebindParentScene(entity, scene);
                scene.Entities.Add(entity);
            }
        }

        var originalFullPath = Path.GetFullPath(originalProjectFilePath);
        var originalName = Path.GetFileNameWithoutExtension(originalFullPath);
        var originalDir = Path.GetDirectoryName(originalFullPath) ?? string.Empty;
        loaded.Name = originalName;
        loaded.Path = originalDir;

        loaded.ClearDirty();
        foreach (var scene in loaded.Scenes)
            scene.ClearDirty();

        return loaded;
    }

    /// <summary>
    /// Drop a small marker file inside the bundle so subsequent
    /// discovery calls skip it. The marker does not change the
    /// bundle's manifest or scene files.
    /// </summary>
    public static void MarkDismissed(AutoSaveBundle bundle)
    {
        if (bundle == null) throw new ArgumentNullException(nameof(bundle));
        var markerPath = Path.Combine(bundle.BundlePath, DismissalMarkerName);
        try
        {
            File.WriteAllText(markerPath, DateTime.UtcNow.ToString("o"));
        }
        catch (Exception ex)
        {
            Logger.Warning($"AutoSave recovery: failed to write dismissal marker for {bundle.BundlePath}: {ex.Message}");
        }
    }

    /// <summary>
    /// Mark a recovered <see cref="Project"/> dirty along with every
    /// <see cref="Scene"/> it contains. Called by the main view
    /// model after assigning a recovered project so the editor
    /// treats the recovery as unsaved work.
    /// </summary>
    public static void MarkRecoveredDirty(Project project)
    {
        if (project == null) return;
        project.MarkDirty();
        foreach (var scene in project.Scenes)
            scene.MarkDirty();
    }

    /// <summary>
    /// Returns the highest UTC write time among the live manifest
    /// and every referenced scene. Returns null when the manifest
    /// is missing, unreadable, invalid, or any referenced scene is
    /// missing or fails to load — which means "no live baseline,
    /// any valid bundle is eligible".
    /// </summary>
    private static DateTime? TryGetLiveBaselineMaxTimestampUtc(string projectFilePath)
    {
        if (string.IsNullOrEmpty(projectFilePath) || !File.Exists(projectFilePath))
            return null;

        Project? project;
        try
        {
            project = Serializer.LoadProject(projectFilePath);
        }
        catch
        {
            return null;
        }
        if (project == null) return null;

        var manifestTs = TryGetFileTimeUtc(projectFilePath);
        if (!manifestTs.HasValue) return null;
        var baseline = manifestTs.Value;

        foreach (var scene in project.Scenes)
        {
            var scenePath = Path.Combine(project.ScenesPath,
                scene.Guid.Value.ToString("D").ToLowerInvariant() + Scene.Extension);
            if (!File.Exists(scenePath))
            {
                return null;
            }
            try
            {
                var probe = Serializer.LoadScene(scenePath);
                if (probe == null) return null;
            }
            catch
            {
                return null;
            }
            var sceneTs = TryGetFileTimeUtc(scenePath);
            if (sceneTs.HasValue && sceneTs.Value > baseline) baseline = sceneTs.Value;
        }

        if (baseline == DateTime.MinValue) return null;
        return baseline;
    }

    private static void RebindParentScene(Components.GameEntity entity, Scene scene)
    {
        entity.ParentScene = scene;
        foreach (var child in entity.Children)
            RebindParentScene(child, scene);
    }

    internal static bool TryParseBundleName(string name, out string projectName, out DateTime timestamp)
    {
        projectName = string.Empty;
        timestamp = default;
        if (string.IsNullOrEmpty(name)) return false;

        var match = BundleNameRegex.Match(name);
        if (!match.Success) return false;

        var parsedProject = match.Groups["project"].Value;
        var tsPart = match.Groups["date"].Value + "_" + match.Groups["time"].Value;
        if (!DateTime.TryParseExact(tsPart, TimestampFormat,
                CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                out var parsed))
        {
            return false;
        }

        if (string.IsNullOrEmpty(parsedProject)) return false;
        projectName = parsedProject;
        timestamp = DateTime.SpecifyKind(parsed, DateTimeKind.Utc);
        return true;
    }

    private static bool ValidateBundle(string bundleDir, string manifestPath)
    {
        if (!File.Exists(manifestPath)) return false;

        Project? project;
        try
        {
            project = Serializer.LoadProject(manifestPath);
        }
        catch
        {
            return false;
        }
        if (project == null) return false;

        foreach (var scene in project.Scenes)
        {
            var scenePath = Path.Combine(bundleDir, "Scenes",
                scene.Guid.Value.ToString("D").ToLowerInvariant() + Scene.Extension);
            if (!File.Exists(scenePath)) return false;
            try
            {
                var loaded = Serializer.LoadScene(scenePath);
                if (loaded == null) return false;
            }
            catch
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsDismissed(string bundleDir)
    {
        try
        {
            return File.Exists(Path.Combine(bundleDir, DismissalMarkerName));
        }
        catch
        {
            return false;
        }
    }

    private static DateTime? TryGetFileTimeUtc(string filePath)
    {
        try
        {
            return File.GetLastWriteTimeUtc(filePath);
        }
        catch
        {
            return null;
        }
    }
}
