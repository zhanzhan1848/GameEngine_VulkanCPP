using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.GameProject;

public static class NewProject
{
    /// <summary>
    /// Sanitize a user-entered project name into something safe to use as
    /// a directory name across Windows / Linux / macOS. Strips control chars
    /// and characters that are illegal in any of those filesystems, replaces
    /// them with '_', trims whitespace, and rejects names that collapse to
    /// empty (".", "..", "   ", all-illegal, etc.).
    /// </summary>
    internal static string SanitizeProjectName(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            throw new ArgumentException("Project name cannot be empty.", nameof(name));
        }

        // Explicit deny-list (not Path.GetInvalidFileNameChars): that call
        // returns only ~2 chars on Linux/macOS, which lets through ":", "*",
        // "?", "<", ">", "|", "\", '"' — all of which are illegal on
        // Windows. Project folders are commonly zipped and shared across
        // platforms, so we sanitize to a superset that's safe everywhere.
        char[] invalidChars = { '<', '>', ':', '"', '/', '\\', '|', '?', '*' };
        var invalidSet = new System.Collections.Generic.HashSet<char>(invalidChars);

        var trimmed = name.Trim();
        var sanitized = new string(trimmed
            .Select(c => invalidSet.Contains(c) || c < 32 ? '_' : c)
            .ToArray());

        // Strip trailing dots/spaces (Windows can't create them, and ".."
        // would point at the parent).
        sanitized = sanitized.TrimEnd('.', ' ');

        if (string.IsNullOrEmpty(sanitized) || sanitized == "." || sanitized == "..")
        {
            throw new ArgumentException(
                $"Project name '{name}' is not a valid folder name.", nameof(name));
        }

        return sanitized;
    }

    public static Project Create(string name, string path, ProjectTemplate template)
    {
        if (template == null)
        {
            throw new ArgumentNullException(nameof(template));
        }
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("Project path cannot be empty.", nameof(path));
        }

        var sanitizedName = SanitizeProjectName(name);
        var projectRoot = Path.Combine(path, sanitizedName);

        // Refuse to clobber an existing folder — protects other projects that
        // happen to live in the same parent directory and prevents the
        // "two projects share a folder" footgun.
        if (Directory.Exists(projectRoot))
        {
            var msg = $"A folder already exists at '{projectRoot}'. " +
                      "Choose a different project name or parent location.";
            Logger.Error(msg);
            throw new IOException(msg);
        }

        try
        {
            Directory.CreateDirectory(projectRoot);
            Directory.CreateDirectory(Path.Combine(projectRoot, "Content"));
            Directory.CreateDirectory(Path.Combine(projectRoot, ".Primal", "Temp"));

            var project = new Project
            {
                Name = sanitizedName,
                Path = projectRoot,
                BuildConfig = 0,
            };

            var scene = new Scene("Default Scene")
            {
                IsActive = true,
            };
            // Default sun light, persisted with the scene on SaveScene below.
            scene.Entities.Add(PrimalEditor_Avalonia.Entities.SceneDefaults.CreateSunLight());
            project.Scenes.Add(scene);
            project.ActiveScene = scene;

            Serializer.SaveProject(project);
            SaveScene(scene, project);

            Logger.Info($"Created new project: {project.FullPath}");
            return project;
        }
        catch (Exception ex)
        {
            // Best-effort cleanup so a half-created folder doesn't linger
            // for the user to stumble into later.
            try
            {
                if (Directory.Exists(projectRoot))
                {
                    Directory.Delete(projectRoot, recursive: true);
                }
            }
            catch (Exception cleanupEx)
            {
                Logger.Warning($"Failed to clean up partial project at '{projectRoot}': {cleanupEx.Message}");
            }

            Logger.Error($"Failed to create new project: {sanitizedName}", ex);
            throw;
        }
    }

    public static void SaveScene(Scene scene, Project project)
    {
        try
        {
            var sceneGuid = scene.Guid.Value.ToString("D").ToLowerInvariant();
            var scenePath = System.IO.Path.Combine(project.ScenesPath, $"{sceneGuid}.scene");
            if (!Directory.Exists(project.ScenesPath))
            {
                Directory.CreateDirectory(project.ScenesPath);
            }
            Serializer.SaveScene(scene, scenePath);
            Logger.Info($"Scene saved: {scenePath}");
        }
        catch (Exception ex)
        {
            Logger.Error($"Failed to save scene: {scene.Name}", ex);
            throw;
        }
    }
}