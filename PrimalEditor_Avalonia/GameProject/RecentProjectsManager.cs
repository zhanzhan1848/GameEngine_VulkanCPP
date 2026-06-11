using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public class RecentProjectsManager
{
    private const int MaxRecent = 10;
    private static readonly string ConfigPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "PrimalEditorAvalonia",
        "recent_projects.json");

    public List<string> RecentProjects { get; private set; } = new();

    public void Load()
    {
        try
        {
            if (File.Exists(ConfigPath))
            {
                var json = File.ReadAllText(ConfigPath);
                RecentProjects = JsonSerializer.Deserialize<List<string>>(json) ?? new();
                Logger.Info($"Loaded {RecentProjects.Count} recent projects");
            }
        }
        catch (Exception ex)
        {
            Logger.Error("Failed to load recent projects", ex);
            RecentProjects = new();
        }
    }

    public void Save()
    {
        try
        {
            var directory = Path.GetDirectoryName(ConfigPath);
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }
            var json = JsonSerializer.Serialize(RecentProjects,
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(ConfigPath, json);
            Logger.Info("Saved recent projects");
        }
        catch (Exception ex)
        {
            Logger.Error("Failed to save recent projects", ex);
        }
    }

    public void Add(string projectPath)
    {
        if (string.IsNullOrEmpty(projectPath)) return;

        RecentProjects.RemoveAll(p => string.Equals(p, projectPath, StringComparison.OrdinalIgnoreCase));
        RecentProjects.Insert(0, projectPath);

        while (RecentProjects.Count > MaxRecent)
        {
            RecentProjects.RemoveAt(RecentProjects.Count - 1);
        }

        Save();
    }
}