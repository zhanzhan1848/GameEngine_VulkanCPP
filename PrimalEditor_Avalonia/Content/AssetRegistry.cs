using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public static class AssetRegistry
{
    private static readonly Dictionary<string, AssetInfo> _byPath = new();
    private static readonly ObservableCollection<AssetInfo> _all = new();

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
        if (File.Exists(fullPath))
        {
            try { info.Hash = ContentHelper.ComputeHash(fullPath); }
            catch (Exception ex) { Logger.Debug($"Hash skipped for {fullPath}: {ex.Message}"); }
        }
        _byPath[fullPath] = info;
        _all.Add(info);
        return info;
    }

    public static void Unregister(string fullPath)
    {
        if (!_byPath.TryGetValue(fullPath, out var info)) return;
        _byPath.Remove(fullPath);
        _all.Remove(info);
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
}