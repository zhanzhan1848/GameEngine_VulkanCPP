using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Undoable hierarchy reparent (plan §6.1 A3). Stores both parents and
/// routes through <see cref="ISceneEntityContext.ReparentEntity"/> so the
/// document model stays the single source of truth.
/// </summary>
public class ReparentEntityAction : IUndoRedoAction
{
    private readonly ISceneEntityContext _context;
    private readonly GameEntity _entity;
    private readonly GameEntity? _oldParent;
    private readonly GameEntity? _newParent;

    public string Name => "Reparent Entity";

    public ReparentEntityAction(ISceneEntityContext context, GameEntity entity,
        GameEntity? newParent)
    {
        _context = context;
        _entity = entity;
        _oldParent = entity.Parent;
        _newParent = newParent;
    }

    public void Undo()
    {
        if (_oldParent != null || _newParent != null)
            _context.ReparentEntity(_entity, _oldParent);
    }

    public void Redo()
    {
        if (_newParent != null || _oldParent != null)
            _context.ReparentEntity(_entity, _newParent);
    }
}

/// <summary>Drag payload for hierarchy reparent drags (same-process
/// DragDrop; no serialization involved).</summary>
public sealed class HierarchyDragData
{
    public GameEntity[] Entities { get; }
    public HierarchyDragData(GameEntity[] entities) => Entities = entities;
}
