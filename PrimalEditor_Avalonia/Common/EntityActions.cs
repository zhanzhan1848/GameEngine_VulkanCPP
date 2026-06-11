using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public class AddEntityAction : IUndoRedoAction
{
    private readonly ISceneEntityContext _context;
    private readonly GameEntity _entity;

    public string Name => "Add Entity";

    public AddEntityAction(ISceneEntityContext context, GameEntity entity)
    {
        _context = context;
        _entity = entity;
    }

    public void Undo()
    {
        _context.RemoveEntity(_entity);
    }

    public void Redo()
    {
        _context.AddEntity(_entity);
    }
}

public class RemoveEntityAction : IUndoRedoAction
{
    private readonly ISceneEntityContext _context;
    private readonly GameEntity _entity;

    public string Name => "Remove Entity";

    public RemoveEntityAction(ISceneEntityContext context, GameEntity entity)
    {
        _context = context;
        _entity = entity;
    }

    public void Undo()
    {
        _context.AddEntity(_entity);
    }

    public void Redo()
    {
        _context.RemoveEntity(_entity);
    }
}