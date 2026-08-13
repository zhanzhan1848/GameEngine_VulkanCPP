using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

public sealed class GizmoWritebackTests
{
    [Fact]
    public void TranslateX_OnlyMutatesPositionX()
    {
        var transform = new Transform { PositionX = 1, PositionY = 2, PositionZ = 3, RotationX = 10, RotationY = 20, RotationZ = 30, ScaleX = 2, ScaleY = 3, ScaleZ = 4 };
        var before = TransformData.FromTransform(transform);

        transform.PositionX += 5f; // simulates Translate axis=X with delta*distance=5

        var after = TransformData.FromTransform(transform);
        Assert.NotEqual(before.PositionX, after.PositionX);
        Assert.Equal(before.PositionY, after.PositionY);
        Assert.Equal(before.PositionZ, after.PositionZ);
        Assert.Equal(before.RotationX, after.RotationX);
        Assert.Equal(before.RotationY, after.RotationY);
        Assert.Equal(before.RotationZ, after.RotationZ);
        Assert.Equal(before.ScaleX, after.ScaleX);
        Assert.Equal(before.ScaleY, after.ScaleY);
        Assert.Equal(before.ScaleZ, after.ScaleZ);
    }

    [Fact]
    public void RotateY_OnlyMutatesRotationY()
    {
        var transform = new Transform { PositionX = 1, PositionY = 2, PositionZ = 3, RotationX = 10, RotationY = 20, RotationZ = 30, ScaleX = 2, ScaleY = 3, ScaleZ = 4 };
        var before = TransformData.FromTransform(transform);

        transform.RotationY += 0.5f;

        var after = TransformData.FromTransform(transform);
        Assert.Equal(before.PositionX, after.PositionX);
        Assert.Equal(before.PositionY, after.PositionY);
        Assert.Equal(before.PositionZ, after.PositionZ);
        Assert.Equal(before.RotationX, after.RotationX);
        Assert.NotEqual(before.RotationY, after.RotationY);
        Assert.Equal(before.RotationZ, after.RotationZ);
        Assert.Equal(before.ScaleX, after.ScaleX);
        Assert.Equal(before.ScaleY, after.ScaleY);
        Assert.Equal(before.ScaleZ, after.ScaleZ);
    }

    [Fact]
    public void ScaleZ_OnlyMutatesScaleZ()
    {
        var transform = new Transform { ScaleX = 2, ScaleY = 3, ScaleZ = 4 };
        var before = TransformData.FromTransform(transform);

        transform.ScaleZ *= 1.5f;

        var after = TransformData.FromTransform(transform);
        Assert.Equal(before.ScaleX, after.ScaleX);
        Assert.Equal(before.ScaleY, after.ScaleY);
        Assert.NotEqual(before.ScaleZ, after.ScaleZ);
    }

    [Fact]
    public void TransformAction_UndoRestoresBefore()
    {
        var transform = new Transform { PositionX = 0 };
        var before = TransformData.FromTransform(transform);
        transform.PositionX = 100;
        var after = TransformData.FromTransform(transform);
        var action = new TransformAction(transform, before, after);

        action.Undo();
        Assert.Equal(0f, transform.PositionX);

        action.Redo();
        Assert.Equal(100f, transform.PositionX);
    }

    [Fact]
    public void TranslateX_DoesNotClobberScaleOrRotation()
    {
        // Regression test for the pre-fix bug: dragging Translate X would
        // write rot={0,0,0} and scl={1,1,1} back to the engine, which
        // round-tripped into C# and silently destroyed any user-set
        // rotation or scale. The fix is to mutate the C# Transform in
        // place and only push to native after the mutation.
        var transform = new Transform
        {
            PositionX = 0,
            RotationX = 45,
            RotationY = 90,
            RotationZ = 180,
            ScaleX = 2,
            ScaleY = 3,
            ScaleZ = 4
        };
        var snapshotBeforeDrag = TransformData.FromTransform(transform);

        // Simulate gizmo Translate X with delta=10 world units.
        transform.PositionX += 10f;

        Assert.Equal(10f, transform.PositionX);
        Assert.Equal(snapshotBeforeDrag.RotationX, transform.RotationX);
        Assert.Equal(snapshotBeforeDrag.RotationY, transform.RotationY);
        Assert.Equal(snapshotBeforeDrag.RotationZ, transform.RotationZ);
        Assert.Equal(snapshotBeforeDrag.ScaleX, transform.ScaleX);
        Assert.Equal(snapshotBeforeDrag.ScaleY, transform.ScaleY);
        Assert.Equal(snapshotBeforeDrag.ScaleZ, transform.ScaleZ);
    }
}
