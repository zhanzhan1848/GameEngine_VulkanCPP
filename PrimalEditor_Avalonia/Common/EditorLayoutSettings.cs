using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace PrimalEditor_Avalonia.Common;

public sealed class WindowState
{
    public int X { get; set; } = 100;
    public int Y { get; set; } = 100;
    public int W { get; set; } = 900;
    public int H { get; set; } = 700;
    /// <summary>0=Normal, 1=Maximized. Stored as int to keep the JSON contract stable.</summary>
    public int State { get; set; }
}

public sealed class EditorLayout
{
    public WindowState MainWindow { get; set; } = new WindowState { W = 1400, H = 900 };
    public List<string> OpenAssets { get; set; } = new();
    public Dictionary<string, WindowState> AssetWindows { get; set; } = new();
}

public static class EditorLayoutSettings
{
    public static string ConfigPath
    {
        get
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "editor-layout.json");
        }
    }

    public static EditorLayout Load()
    {
        try
        {
            var path = ConfigPath;
            if (!File.Exists(path)) return new EditorLayout();
            using var fs = File.OpenRead(path);
            return JsonSerializer.Deserialize<EditorLayout>(fs, Options) ?? new EditorLayout();
        }
        catch (Exception ex)
        {
            Logger.Warning($"EditorLayoutSettings.Load: {ex.Message}; using defaults");
            return new EditorLayout();
        }
    }

    public static bool Save(EditorLayout layout)
    {
        if (layout == null) return false;
        var path = ConfigPath;
        var partial = path + ".partial";
        try
        {
            using (var fs = File.Create(partial))
            {
                JsonSerializer.Serialize(fs, layout, Options);
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(partial, path);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"EditorLayoutSettings.Save failed: {path}", ex);
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            return false;
        }
    }

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
    };
}
