using System;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class LightIconRendererTests
{
    [Fact]
    public void DrawSceneIcons_NoScene_DoesNotThrow()
    {
        var r = new LightIconRenderer();
        r.UpdateCamera(yaw: 0, pitch: 0, distance: 12);
        r.DrawSceneIcons(null);
    }

    [Fact]
    public void DrawSceneIcons_SceneWithoutLights_DoesNotThrow()
    {
        var r = new LightIconRenderer();
        r.UpdateCamera(yaw: 0, pitch: 0, distance: 12);
        var scene = new Scene("Main");
        scene.Entities.Add(new GameEntity("Empty"));
        r.DrawSceneIcons(scene);
    }

    [Fact]
    public void WorldProjector_Basis_RoundTripsThroughUpdateCamera()
    {
        var basis = new WorldProjector.Basis();
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: 0, pitch: 0, distance: 10,
            basis);

        // Native camera convention: yaw=0, pitch=0 → local +Z forward.
        Assert.Equal(0f, basis.ForwardX, 3);
        Assert.Equal(0f, basis.ForwardY, 3);
        Assert.Equal(1f, basis.ForwardZ, 3);
        // Camera position = target - forward*distance = (0, 0, -10).
        Assert.Equal(0f, basis.CamX, 3);
        Assert.Equal(0f, basis.CamY, 3);
        Assert.Equal(-10f, basis.CamZ, 3);
        // WorldUp × Forward produces camera-right (+X).
        Assert.Equal(1f, basis.RightX, 3);
        Assert.Equal(0f, basis.RightY, 3);
        Assert.Equal(0f, basis.RightZ, 3);
    }

    [Fact]
    public void WorldProjector_Basis_RotatesWithYaw()
    {
        var basis = new WorldProjector.Basis();
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: MathF.PI / 2f, pitch: 0, distance: 10,
            basis);

        // Yaw=π/2 rotates native +Z forward to +X.
        Assert.Equal(1f, basis.ForwardX, 3);
        Assert.Equal(0f, basis.ForwardY, 3);
        Assert.Equal(0f, basis.ForwardZ, 3);
    }
}
