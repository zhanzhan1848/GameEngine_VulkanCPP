using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public static class ProjectEditorSettings
{
    public static string Path2(Project project)
    {
        var dir = string.IsNullOrEmpty(project?.Path)
            ? Path.GetTempPath()
            : project.Path;
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "EditorSettings.json");
    }

    public static JsonObject? Read(Project project)
    {
        var p = Path2(project);
        if (!File.Exists(p)) return null;
        try
        {
            using var fs = File.OpenRead(p);
            return JsonNode.Parse(fs) as JsonObject;
        }
        catch (Exception ex)
        {
            Logger.Warning($"ProjectEditorSettings.Read failed: {ex.Message}");
            return null;
        }
    }

    public static bool Write(Project project, JsonObject root)
    {
        var p = Path2(project);
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
            Logger.Error($"ProjectEditorSettings.Write failed: {p}", ex);
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            return false;
        }
    }

    public static JsonObject? GetImporterSettings(Project project, string importerName)
    {
        var root = Read(project);
        return root?[importerName] as JsonObject;
    }

    public static void SetImporterSettings(Project project, string importerName, JsonObject settings)
    {
        var root = Read(project) ?? new JsonObject();
        root[importerName] = settings;
        Write(project, root);
    }
}
