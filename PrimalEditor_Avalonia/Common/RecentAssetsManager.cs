using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Common;

public static class RecentAssetsManager
{
    public const int MaxItems = 10;

    private static readonly List<string> _paths = new();
    private static bool _loaded;

    public static IReadOnlyList<string> RecentPaths
    {
        get { EnsureLoaded(); return _paths.AsReadOnly(); }
    }

    public static void Add(string fullPath)
    {
        if (string.IsNullOrEmpty(fullPath)) return;
        EnsureLoaded();
        // Move-to-front dedup: if the path is already in the list, drop the
        // older copy and re-prepend so most-recently-used sorts first.
        _paths.RemoveAll(p => string.Equals(p, fullPath, StringComparison.OrdinalIgnoreCase));
        _paths.Insert(0, fullPath);
        while (_paths.Count > MaxItems) _paths.RemoveAt(_paths.Count - 1);
        Save();
    }

    public static void Clear() { _paths.Clear(); Save(); }

    private static string ConfigPath
    {
        get
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "recent-assets.json");
        }
    }

    private static void EnsureLoaded()
    {
        if (_loaded) return;
        _loaded = true;
        try
        {
            var path = ConfigPath;
            if (!File.Exists(path)) return;
            using var fs = File.OpenRead(path);
            var list = JsonSerializer.Deserialize<List<string>>(fs);
            if (list != null)
            {
                _paths.Clear();
                _paths.AddRange(list);
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"RecentAssetsManager.Load: {ex.Message}");
        }
    }

    public static void Save()
    {
        try
        {
            var path = ConfigPath;
            var partial = path + ".partial";
            using (var fs = File.Create(partial))
            {
                JsonSerializer.Serialize(fs, _paths, new JsonSerializerOptions { WriteIndented = true });
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(partial, path);
        }
        catch (Exception ex)
        {
            Logger.Warning($"RecentAssetsManager.Save: {ex.Message}");
        }
    }
}
