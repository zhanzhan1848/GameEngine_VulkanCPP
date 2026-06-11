using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public static class Serializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        IncludeFields = false,
    };

    public static void SaveProject(Project project, string? path = null)
    {
        var filePath = path ?? project.FullPath;
        try
        {
            var directory = Path.GetDirectoryName(filePath);
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
            {
                Directory.CreateDirectory(directory);
            }

            var json = JsonSerializer.Serialize(project, Options);
            File.WriteAllText(filePath, json);
            Logger.Info($"Project saved: {filePath}");
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to save project: {filePath}", ex);
            throw;
        }
    }

    public static Project? LoadProject(string filePath)
    {
        try
        {
            if (!File.Exists(filePath))
            {
                Logger.Error($"Project file not found: {filePath}");
                return null;
            }

            var json = File.ReadAllText(filePath);
            var project = JsonSerializer.Deserialize<Project>(json, Options);
            if (project != null)
            {
                Logger.Info($"Project loaded: {filePath}");
            }
            return project;
        }
        catch (JsonException ex)
        {
            Logger.Error($"Project file is corrupted: {filePath}", ex);
            return null;
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to load project: {filePath}", ex);
            return null;
        }
    }
}