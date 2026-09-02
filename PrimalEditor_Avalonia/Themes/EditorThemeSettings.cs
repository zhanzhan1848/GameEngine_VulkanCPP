using System;
using System.IO;
using System.Text.Json;

namespace PrimalEditor_Avalonia.Themes;

public sealed class EditorThemeConfig
{
    public EditorThemeConfig() { }
    public EditorThemeConfig(string themeId) { ThemeId = themeId; }
    public string ThemeId { get; set; } = EditorThemeRegistry.DefaultThemeId;
}

/// <summary>
/// Persists the active theme id to <c>%APPDATA%/PrimalEditorAvalonia/editor-theme.json</c>,
/// following the same atomic-write pattern as <see cref="Common.EditorLayoutSettings"/>.
/// Corrupt or missing files fall back to the default theme.
/// </summary>
public static class EditorThemeSettings
{
    public static string ConfigPath
    {
        get
        {
            // Tests (and sandboxed runs) redirect the file via env var so
            // switching themes in a test never rewrites the user's real
            // editor-theme.json.
            var env = Environment.GetEnvironmentVariable("PRIMAL_EDITOR_THEME_FILE");
            if (!string.IsNullOrWhiteSpace(env))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(env)!);
                return env;
            }
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "editor-theme.json");
        }
    }

    public static EditorThemeConfig Load()
    {
        try
        {
            var path = ConfigPath;
            if (!File.Exists(path)) return new EditorThemeConfig();
            using var fs = File.OpenRead(path);
            return JsonSerializer.Deserialize<EditorThemeConfig>(fs, Options) ?? new EditorThemeConfig();
        }
        catch (Exception ex)
        {
            Common.Logger.Warning($"EditorThemeSettings.Load: {ex.Message}; using defaults");
            return new EditorThemeConfig();
        }
    }

    public static bool Save(EditorThemeConfig config)
    {
        if (config == null) return false;
        var path = ConfigPath;
        var partial = path + ".partial";
        try
        {
            using (var fs = File.Create(partial))
            {
                JsonSerializer.Serialize(fs, config, Options);
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(partial, path);
            return true;
        }
        catch (Exception ex)
        {
            Common.Logger.Error($"EditorThemeSettings.Save failed: {path}", ex);
            return false;
        }
    }

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
    };
}
