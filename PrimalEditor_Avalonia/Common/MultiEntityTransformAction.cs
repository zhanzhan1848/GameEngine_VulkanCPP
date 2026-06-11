using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Common;

public class MultiEntityTransformAction : IUndoRedoAction
{
    private readonly List<(GameEntity entity, TransformData before)> _before;
    private readonly List<(GameEntity entity, TransformData after)> _after;
    private readonly string _name;

    public string Name => _name;

    public MultiEntityTransformAction(
        List<(GameEntity entity, TransformData before)> before,
        List<(GameEntity entity, TransformData after)> after,
        string name = "Transform Edit")
    {
        _before = before;
        _after = after;
        _name = name;
    }

    public void Undo()
    {
        foreach (var (entity, data) in _before)
        {
            var t = entity.GetTransform();
            if (t != null) data.ApplyTo(t);
        }
    }

    public void Redo()
    {
        foreach (var (entity, data) in _after)
        {
            var t = entity.GetTransform();
            if (t != null) data.ApplyTo(t);
        }
    }
}