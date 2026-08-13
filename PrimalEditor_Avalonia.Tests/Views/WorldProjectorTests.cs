using System;
using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class WorldProjectorTests
{
    [Fact]
    public void PerspectiveFocalLength_MatchesNativeSixtyDegreeVerticalFov()
    {
        var focal = WorldProjector.PerspectiveFocalLength(600f);

        Assert.Equal(300f / MathF.Tan(MathF.PI / 6f), focal, 4);
        Assert.True(focal > 500f); // Guards against the old implicit 90° FOV.
    }

    [Fact]
    public void Project_OriginAtCameraTarget_ProjectsToViewportCenter()
    {
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: 0, pitch: 0, distance: 10,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);

        // World origin is 10 units in front of the native +Z-forward camera.
        var sp = WorldProjector.Project(
            0f, 0f, 0f,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            800, 600, 300);
        Assert.NotNull(sp);
        Assert.Equal(400f, sp!.Value.sx);  // half of 800
        Assert.Equal(300f, sp.Value.sy);  // half of 600
    }

    [Fact]
    public void Project_PointBehindCamera_ReturnsNull()
    {
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: 0, pitch: 0, distance: 10,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);

        // Camera is at (0,0,-10) with forward = +Z. A point at z=-11
        // is behind the camera.
        var sp = WorldProjector.Project(
            0f, 0f, -11f,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            800, 600, 300);
        Assert.Null(sp);
    }

    [Fact]
    public void Project_OneUnitRightAtTenUnitsForward_ShiftsOneThirdWidth()
    {
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: 0, pitch: 0, distance: 10,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);

        // focal = 300, viewport width = 800.
        // (1 / 10) * 300 = 30 → sx = 400 + 30 = 430.
        var sp = WorldProjector.Project(
            1f, 0f, 0f,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            800, 600, 300);
        Assert.NotNull(sp);
        Assert.Equal(430f, sp!.Value.sx, 0);
        Assert.Equal(300f, sp.Value.sy, 0);
    }

    [Fact]
    public void Project_PointAboveCameraTarget_ShiftsUpwardOnScreen()
    {
        WorldProjector.ComputeCameraBasis(
            targetX: 0, targetY: 0, targetZ: 0,
            yaw: 0, pitch: 0, distance: 10,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);

        // A point at (0,1,0) should project upward on screen
        // (screen-y decreases → sp.sy < viewportH/2).
        var sp = WorldProjector.Project(
            0f, 1f, 0f,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            800, 600, 300);
        Assert.NotNull(sp);
        Assert.True(sp!.Value.sy < 300f);  // up = smaller sy
    }

    [Theory]
    [InlineData(0.0f, 0.0f)]
    [InlineData(0.75f, 0.35f)]
    [InlineData(-1.628f, -0.814f)]
    public void CameraTransform_NativeForwardPointsAtTarget(float yaw, float pitch)
    {
        WorldProjector.ComputeCameraTransform(
            1.5f, -2f, 3.25f, yaw, pitch, 12f,
            out var px, out var py, out var pz,
            out var rx, out var ry, out var rz);

        // Native applies Z * Y * X to local +Z. rotZ is deliberately zero.
        var sinX = MathF.Sin(rx);
        var cosX = MathF.Cos(rx);
        var sinY = MathF.Sin(ry);
        var cosY = MathF.Cos(ry);
        var forwardX = sinY * cosX;
        var forwardY = -sinX;
        var forwardZ = cosY * cosX;
        var toTargetX = (1.5f - px) / 12f;
        var toTargetY = (-2f - py) / 12f;
        var toTargetZ = (3.25f - pz) / 12f;

        Assert.Equal(0f, rz, 6);
        Assert.Equal(toTargetX, forwardX, 5);
        Assert.Equal(toTargetY, forwardY, 5);
        Assert.Equal(toTargetZ, forwardZ, 5);
    }
}
