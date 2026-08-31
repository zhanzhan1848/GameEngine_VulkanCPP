using System.Linq;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public class DuplicateEntityAction : IUndoRedoAction
{
    private readonly ISceneEntityContext _context;
    private readonly GameEntity _original;
    private GameEntity? _duplicate;

    public string Name => "Duplicate Entity";

    public DuplicateEntityAction(ISceneEntityContext context, GameEntity original)
    {
        _context = context;
        _original = original;
    }

    public void Undo()
    {
        if (_duplicate != null)
        {
            _context.RemoveEntity(_duplicate);
        }
    }

    public void Redo()
    {
        _duplicate = CloneEntity(_original);
        _context.AddEntity(_duplicate);
    }

    private static GameEntity CloneEntity(GameEntity source)
    {
        var clone = new GameEntity(source.Name + "_Copy")
        {
            IsEnabled = source.IsEnabled,
            IsActive = source.IsActive,
        };

        foreach (var component in source.Components)
        {
            if (component is Transform t)
            {
                clone.Components.Add(new Transform
                {
                    PositionX = t.PositionX,
                    PositionY = t.PositionY,
                    PositionZ = t.PositionZ,
                    RotationX = t.RotationX,
                    RotationY = t.RotationY,
                    RotationZ = t.RotationZ,
                    ScaleX = t.ScaleX,
                    ScaleY = t.ScaleY,
                    ScaleZ = t.ScaleZ,
                });
            }
        }

        return clone;
    }
}