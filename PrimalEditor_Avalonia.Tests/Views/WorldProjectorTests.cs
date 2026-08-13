using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class WorldProjectorTests
{
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

        // World origin (0,0,0) is 10 units in front of camera at (0,0,10).
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

        // Camera is at (0,0,10) with forward = -Z. A point at z=11
        // is behind the camera (z > cam_z); a point at z=9 is in
        // front.
        var sp = WorldProjector.Project(
            0f, 0f, 11f,
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
}