using System;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public static class NewProject
{
    public static Project Create(string name, string path, ProjectTemplate template)
    {
        try
        {
            Directory.CreateDirectory(path);
            Directory.CreateDirectory(Path.Combine(path, "Content"));
            Directory.CreateDirectory(Path.Combine(path, ".Primal", "Temp"));

            var project = new Project
            {
                Name = name,
                Path = path,
                BuildConfig = 0,
            };

            var scene = new Scene("Default Scene")
            {
                IsActive = true,
            };
            project.Scenes.Add(scene);
            project.ActiveScene = scene;

            Serializer.SaveProject(project);
            SaveScene(scene, project);

            Logger.Info($"Created new project: {project.FullPath}");
            return project;
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to create new project: {name}", ex);
            throw;
        }
    }

    public static void SaveScene(Scene scene, Project project)
    {
        try
        {
            var scenePath = Path.Combine(project.ScenesPath, scene.Name + ".scene");
            if (!Directory.Exists(project.ScenesPath))
            {
                Directory.CreateDirectory(project.ScenesPath);
            }
            var json = System.Text.Json.JsonSerializer.Serialize(scene,
                new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(scenePath, json);
            Logger.Info($"Scene saved: {scenePath}");
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to save scene: {scene.Name}", ex);
            throw;
        }
    }
}