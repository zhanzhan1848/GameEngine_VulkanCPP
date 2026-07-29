using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

public interface IEditorSaveService
{
    bool SaveProject(Project project, string? path = null);
    bool SaveScene(Project project, Scene scene);
    bool SaveAll(Project project);
}
