using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public static class OpenProject
{
    public static Project? Open(string filePath)
    {
        if (!File.Exists(filePath))
        {
            Logger.Error($"Project file not found: {filePath}");
            return null;
        }

        var project = Serializer.LoadProject(filePath);
        if (project == null)
        {
            return null;
        }

        if (!string.IsNullOrEmpty(project.Path))
        {
            Directory.CreateDirectory(project.ContentPath);
            Directory.CreateDirectory(project.ScenesPath);
            Directory.CreateDirectory(project.TempFolder);
        }

        Logger.Info($"Project opened: {project.FullPath}");
        return project;
    }
}