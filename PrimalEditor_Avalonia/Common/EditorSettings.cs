using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PrimalEditor_Avalonia.Common;

public static class EditorSettings
{
    public static string Path2
    {
        get
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "editor-settings.json");
        }
    }

    public static JsonObject? Read()
    {
        var p = Path2;
        if (!File.Exists(p)) return null;
        try
        {
            using var fs = File.OpenRead(p);
            return JsonNode.Parse(fs) as JsonObject;
        }
        catch (Exception ex)
        {
            Logger.Warning($"EditorSettings.Read failed: {ex.Message}");
            return null;
        }
    }

    public static bool Write(JsonObject root)
    {
        var p = Path2;
        var partial = p + ".partial";
        try
        {
            using (var fs = File.Create(partial))
            {
                using var writer = new Utf8JsonWriter(fs, new JsonWriterOptions { Indented = true });
                root.WriteTo(writer);
            }
            if (File.Exists(p)) File.Delete(p);
            File.Move(partial, p);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"EditorSettings.Write failed: {p}", ex);
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            return false;
        }
    }

    public static JsonObject? GetImporterSettings(string importerName)
    {
        var root = Read();
        return root?[importerName] as JsonObject;
    }

    public static void SetImporterSettings(string importerName, JsonObject settings)
    {
        var root = Read() ?? new JsonObject();
        root[importerName] = settings;
        Write(root);
    }
}
