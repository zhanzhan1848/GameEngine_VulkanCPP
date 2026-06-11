using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public class TransformAction : IUndoRedoAction
{
    private readonly Transform _transform;
    private readonly TransformData _oldData;
    private readonly TransformData _newData;

    public string Name => "Transform Edit";

    public TransformAction(Transform transform, TransformData oldData, TransformData newData)
    {
        _transform = transform;
        _oldData = oldData;
        _newData = newData;
    }

    public void Undo()
    {
        ApplyData(_oldData);
    }

    public void Redo()
    {
        ApplyData(_newData);
    }

    private void ApplyData(TransformData data)
    {
        _transform.PositionX = data.PositionX;
        _transform.PositionY = data.PositionY;
        _transform.PositionZ = data.PositionZ;
        _transform.RotationX = data.RotationX;
        _transform.RotationY = data.RotationY;
        _transform.RotationZ = data.RotationZ;
        _transform.ScaleX = data.ScaleX;
        _transform.ScaleY = data.ScaleY;
        _transform.ScaleZ = data.ScaleZ;
    }
}

public struct TransformData
{
    public float PositionX;
    public float PositionY;
    public float PositionZ;
    public float RotationX;
    public float RotationY;
    public float RotationZ;
    public float ScaleX;
    public float ScaleY;
    public float ScaleZ;

    public static TransformData FromTransform(Transform t)
    {
        return new TransformData
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
        };
    }

    public void ApplyTo(Transform t)
    {
        t.PositionX = PositionX;
        t.PositionY = PositionY;
        t.PositionZ = PositionZ;
        t.RotationX = RotationX;
        t.RotationY = RotationY;
        t.RotationZ = RotationZ;
        t.ScaleX = ScaleX;
        t.ScaleY = ScaleY;
        t.ScaleZ = ScaleZ;
    }
}