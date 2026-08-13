using System;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class SceneIconGeometryTests
{
    [Fact]
    public void TransformPoint_FollowsTranslationAndNonUniformScale()
    {
        var transform = new Transform
        {
            PositionX = 10,
            PositionY = 20,
            PositionZ = 30,
            ScaleX = 2,
            ScaleY = 3,
            ScaleZ = 4,
        };

        var point = SceneIconGeometry.TransformPoint(transform, 1, 1, 1);

        Assert.Equal(12f, point.x, 4);
        Assert.Equal(23f, point.y, 4);
        Assert.Equal(34f, point.z, 4);
    }

    [Fact]
    public void TransformPoint_FollowsEntityEulerRotation()
    {
        var transform = new Transform { RotationZ = MathF.PI / 2f };

        var point = SceneIconGeometry.TransformPoint(transform, 1, 0, 0);

        Assert.Equal(0f, point.x, 4);
        Assert.Equal(1f, point.y, 4);
        Assert.Equal(0f, point.z, 4);
    }
}
