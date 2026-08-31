using System;
using Avalonia;
using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class GizmoOverlayTests
{
    [Fact]
    public void TryStartDrag_HitsNativeSizedYAxisAtSixtyDegreeFov()
    {
        const float width = 800f;
        const float height = 600f;
        const float yaw = 0.75f;
        const float pitch = -0.25f;
        const float distance = 12f;
        const float targetX = 5f;

        var gizmo = new GizmoOverlay { IsVisibleInViewport = true };
        gizmo.Measure(new Size(width, height));
        gizmo.Arrange(new Rect(0, 0, width, height));
        gizmo.UpdateCamera(yaw, pitch, distance, 0f, 0f, 0f, width, height);
        gizmo.SetTarget(targetX, 0f, 0f);

        WorldProjector.ComputeCameraBasis(
            0f, 0f, 0f, yaw, pitch, distance,
            out var camX, out var camY, out var camZ,
            out var rightX, out var rightY, out var rightZ,
            out var upX, out var upY, out var upZ,
            out var forwardX, out var forwardY, out var forwardZ);
        var focal = WorldProjector.PerspectiveFocalLength(height);
        var worldLength = Math.Clamp(distance * 0.12f, 0.65f, 4f);
        var center = WorldProjector.Project(
            targetX, 0f, 0f,
            camX, camY, camZ,
            rightX, rightY, rightZ, upX, upY, upZ,
            forwardX, forwardY, forwardZ, width, height, focal)!.Value;
        var yEnd = WorldProjector.Project(
            targetX, worldLength, 0f,
            camX, camY, camZ,
            rightX, rightY, rightZ, upX, upY, upZ,
            forwardX, forwardY, forwardZ, width, height, focal)!.Value;
        var midpoint = new Point(
            (center.sx + yEnd.sx) * 0.5f,
            (center.sy + yEnd.sy) * 0.5f);

        Assert.True(gizmo.TryStartDrag(midpoint));
        Assert.True(gizmo.IsDragging);

        (GizmoOverlay.GizmoMode mode, GizmoOverlay.GizmoAxis axis, float delta)? emitted = null;
        gizmo.OnDelta += (_, value) => emitted = value;
        gizmo.DragDelta(new Point(
            midpoint.X + (yEnd.sx - center.sx) * 0.25f,
            midpoint.Y + (yEnd.sy - center.sy) * 0.25f));

        Assert.NotNull(emitted);
        Assert.Equal(GizmoOverlay.GizmoAxis.Y, emitted!.Value.axis);
        Assert.True(emitted.Value.delta > 0f);
    }
}
