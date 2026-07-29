using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

public sealed class EditorSaveService : IEditorSaveService
{
    public static EditorSaveService Instance { get; } = new();

    private EditorSaveService() { }

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
            Logger.Error("EditorSaveService.SaveScene failed", ex);
            return false;
        }
    }

    public bool SaveAll(Project project)
    {
        if (project == null) return false;
        bool ok = SaveProject(project);
        foreach (var scene in project.Scenes)
        {
            if (!SaveScene(project, scene)) ok = false;
        }
        return ok;
    }
}
