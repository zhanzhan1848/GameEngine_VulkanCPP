using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

public sealed class EditorSaveService : IEditorSaveService
{
    public static EditorSaveService Instance { get; } = new();

    public static IReadOnlyList<string> LastSaveAllFailures { get; private set; } = Array.Empty<string>();

    public EditorSaveService() { }

    public bool SaveProject(Project project, string? path = null)
    {
        if (project == null) return false;
        try
        {
            Serializer.SaveProject(project, path);
            project.ClearDirty();
            return true;
        }
        catch (System.Exception ex)
        {
            Logger.Error("EditorSaveService.SaveProject failed", ex);
            return false;
        }
    }

    public bool SaveScene(Project project, Scene scene)
    {
        if (project == null || scene == null) return false;
        try
        {
            NewProject.SaveScene(scene, project);
            scene.ClearDirty();
            return true;
        }
        catch (System.Exception ex)
        {
            Logger.Error($"EditorSaveService.SaveScene failed: {scene.Name}", ex);
            return false;
        }
    }

    public bool SaveAll(Project project)
    {
        if (project == null) return false;

        bool ok = true;
        var failures = new List<string>();

        foreach (var scene in project.Scenes)
        {
            if (!SaveScene(project, scene))
            {
                ok = false;
                failures.Add($"scene '{scene.Name}' could not be saved; see logs");
            }
        }

        // Only persist the manifest if every scene succeeded — otherwise
        // leave IsDirty set so the next close attempt prompts again and
        // we don't create a manifest pointing at half-written scenes.
        if (ok)
        {
            try
            {
                if (!SaveProject(project)) ok = false;
            }
            catch (System.Exception ex)
            {
                ok = false;
                failures.Add($"project manifest: {ex.Message}");
            }
        }

        LastSaveAllFailures = failures;
        return ok;
    }
}
