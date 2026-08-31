using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public class RenameEntityAction : IUndoRedoAction
{
    private readonly GameEntity _entity;
    private readonly string _newName;
    private readonly string _oldName;

    public string Name => $"Rename '{_oldName}' to '{_newName}'";

    public RenameEntityAction(GameEntity entity, string newName)
    {
        _entity = entity;
        _newName = newName;
        _oldName = entity.Name;
    }

    public void Undo()
    {
        _entity.Name = _oldName;
    }

    public void Redo()
    {
        _entity.Name = _newName;
    }
}