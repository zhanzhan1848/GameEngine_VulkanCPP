using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public static class AssetRegistry
{
    private static readonly Dictionary<string, AssetInfo> _byPath = new();
    private static readonly ObservableCollection<AssetInfo> _all = new();

    // Persisted GUID cache: project-relative path → GUID. Populated
    // when a project is opened and re-applied during Register so the
    // same asset keeps the same identity across editor sessions.
    private static readonly Dictionary<string, Guid> _persistedGuids = new();
    private static string? _persistedGuidsPath;

    public static ReadOnlyObservableCollection<AssetInfo> Assets { get; } =
        new ReadOnlyObservableCollection<AssetInfo>(_all);

    public static int Count => _all.Count;

    public static void Initialize(string projectContentRoot)
    {
        Reset();
        if (!Directory.Exists(projectContentRoot))
        {
            Logger.Warning($"AssetRegistry: Content root not found: {projectContentRoot}");
            return;
        }
        ScanRecursive(projectContentRoot);
        Logger.Info($"AssetRegistry: scanned {_all.Count} asset(s) from {projectContentRoot}");
    }

    public static void Reset()
    {
        _byPath.Clear();
        _all.Clear();
    }

    public static AssetInfo? Register(string fullPath)
    {
        if (!File.Exists(fullPath) && !Directory.Exists(fullPath)) return null;
        if (_byPath.TryGetValue(fullPath, out var existing)) return existing;

        var info = new AssetInfo
        {
            FullPath = fullPath,
            Type = ContentHelper.IsDirectory(fullPath)
                ? AssetType.Unknown
                : ContentHelper.GetAssetType(fullPath),
            RegisterTime = DateTime.UtcNow,
            ImportDate = File.Exists(fullPath)
                ? File.GetLastWriteTimeUtc(fullPath)
                : DateTime.UtcNow,
        };
        // Reuse a previously-persisted GUID for this path so the asset
        // identity is stable across editor sessions.
        if (_persistedGuids.TryGetValue(fullPath, out var persisted))
        {
            info.Guid = persisted;
        }
        else
        {
            _persistedGuids[fullPath] = info.Guid;
            SchedulePersist();
        }
        if (File.Exists(fullPath))
        {
            try { info.Hash = ContentHelper.ComputeHash(fullPath); }
            catch (Exception ex) { Logger.Debug($"Hash skipped for {fullPath}: {ex.Message}"); }
        }
        _byPath[fullPath] = info;
        _all.Add(info);
        return info;
    }

    private static System.Timers.Timer? _persistTimer;
    private static void SchedulePersist()
    {
        // Debounce: many Register calls during a single scan shouldn't
        // each trigger a full JSON write. Coalesce into one write per
        // 2-second window.
        if (_persistTimer != null) return;
        _persistTimer = new System.Timers.Timer(2000) { AutoReset = false };
        _persistTimer.Elapsed += (_, _) =>
        {
            _persistTimer = null;
            PersistGuids();
        };
        _persistTimer.Start();
    }

    public static void Unregister(string fullPath)
    {
        if (!_byPath.TryGetValue(fullPath, out var info)) return;
        _byPath.Remove(fullPath);
        _all.Remove(info);
        _persistedGuids.Remove(fullPath);
        PersistGuids();
    }

    public static AssetInfo? Get(string fullPath) =>
        _byPath.TryGetValue(fullPath, out var i) ? i : null;

    public static IEnumerable<AssetInfo> GetByType(AssetType type)
    {
        foreach (var info in _all)
            if (info.Type == type) yield return info;
    }

    private static void ScanRecursive(string root)
    {
        foreach (var dir in Directory.GetDirectories(root))
            ScanRecursive(dir);
        foreach (var file in Directory.GetFiles(root))
        {
            if (ContentHelper.IsAssetFile(file)) Register(file);
        }
    }

    private static string GetGuidsFileForProject(string projectContentRoot)
    {
        // Sidecar JSON lives in the project's .primal folder so it
        // travels with the project, not the editor.
        var dir = Path.Combine(Path.GetDirectoryName(projectContentRoot) ?? projectContentRoot, ".primal");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "asset-guids.json");
    }

    private static void LoadPersistedGuidsForProject(string projectContentRoot)
    {
        _persistedGuidsPath = GetGuidsFileForProject(projectContentRoot);
        _persistedGuids.Clear();
        try
        {
            if (!File.Exists(_persistedGuidsPath)) return;
            using var fs = File.OpenRead(_persistedGuidsPath);
            var dict = JsonSerializer.Deserialize<Dictionary<string, string>>(fs);
            if (dict != null)
            {
                foreach (var (k, v) in dict)
                {
                    if (Guid.TryParse(v, out var g)) _persistedGuids[k] = g;
                }
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"AssetRegistry: failed to load {Path.GetFileName(_persistedGuidsPath)}: {ex.Message}");
        }
    }

    private static void PersistGuids()
    {
        if (string.IsNullOrEmpty(_persistedGuidsPath)) return;
        try
        {
            var partial = _persistedGuidsPath + ".partial";
            using (var fs = File.Create(partial))
            {
                JsonSerializer.Serialize(fs, _persistedGuids.ToDictionary(kv => kv.Key, kv => kv.Value.ToString()));
            }
            if (File.Exists(_persistedGuidsPath)) File.Delete(_persistedGuidsPath);
            File.Move(partial, _persistedGuidsPath);
        }
        catch (Exception ex)
        {
            Logger.Warning($"AssetRegistry: failed to persist {Path.GetFileName(_persistedGuidsPath)}: {ex.Message}");
        }
    }
}