using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Composite managed overlay drawn on top of the engine viewport.
/// Renders the world-space helpers (grid floor + world axes) plus an
/// optional gizmo for the currently selected entity. World-space
/// geometry is projected through the same camera math as
/// <see cref="EngineViewport3D"/>.
///
/// This is a managed-only overlay: it does not push any geometry to
/// the engine GPU. The grid and axes are drawn in screen space after
/// world-to-screen projection.
/// </summary>
public class ViewportOverlay : Control
{
    /// <summary>World position of the gizmo target (selected entity).</summary>
    public (float x, float y, float z) GizmoTarget { get; set; } = (0, 0, 0);

    /// <summary>Camera orbit state (mirrored from EngineViewport3D).</summary>
    public float CameraYaw { get; set; }
    public float CameraPitch { get; set; }
    public float CameraDistance { get; set; }
    public (float x, float y, float z) CameraTarget { get; set; } = (0, 0, 0);

    /// <summary>Show the grid floor (xz plane at y=0). Default true.</summary>
    public bool ShowGrid { get; set; } = true;

    /// <summary>Show the world axes (X red, Y green, Z blue). Default true.</summary>
    public bool ShowAxes { get; set; } = true;

    /// <summary>Show the gizmo. Default true.</summary>
    public bool ShowGizmo { get; set; } = true;

    /// <summary>
    /// Grid extent in world units. Lines are drawn from
    /// <c>-Extent</c> to <c>+Extent</c> at integer steps of
    /// <c>Step</c>. Default: 20m extent, 1m step.
    /// </summary>
    public float GridExtent { get; set; } = 20f;
    public float GridStep { get; set; } = 1f;

    public override void Render(DrawingContext ctx)
    {
        var b = Bounds;
        if (b.Width <= 4 || b.Height <= 4) return;
        float w = (float)b.Width, h = (float)b.Height;
        float focal = MathF.Min(w, h) * 0.5f;
        float cyA = MathF.Cos(CameraYaw), syA = MathF.Sin(CameraYaw);
        float cpA = MathF.Cos(CameraPitch), spA = MathF.Sin(CameraPitch);
        // Camera forward: yaw=0 → -Z (camera at +Z relative to target).
        float ox = -syA * cpA, oy = -spA, oz = -cyA * cpA;
        // Camera position in world.
        float camX = CameraTarget.x - ox * CameraDistance;
        float camY = CameraTarget.y - oy * CameraDistance;
        float camZ = CameraTarget.z - oz * CameraDistance;
        // Camera basis (right, up, forward) for projecting world XZ to screen.
        float[] forward = new[] { ox, oy, oz };
        float[] worldUp = new[] { 0f, 1f, 0f };
        float rx = forward[1] * worldUp[2] - forward[2] * worldUp[1];
        float ry = forward[2] * worldUp[0] - forward[0] * worldUp[2];
        float rz = forward[0] * worldUp[1] - forward[1] * worldUp[0];
        float rlen = MathF.Sqrt(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-6f) { rx /= rlen; ry /= rlen; rz /= rlen; }
        float ux = ry * forward[2] - rz * forward[1];
        float uy = rz * forward[0] - rx * forward[2];
        float uz = rx * forward[1] - ry * forward[0];
        // Project a world-space point to screen coords.
        // Returns null if the point is behind the camera (z >= 0).
        (float sx, float sy)? Project(float px, float py, float pz)
        {
            float dx = px - camX, dy = py - camY, dz = pz - camZ;
            float cu = dx * rx + dy * ry + dz * rz;
            float cv = dx * ux + dy * uy + dz * uz;
            float cw = dx * forward[0] + dy * forward[1] + dz * forward[2];
            if (cw < 0.05f) return null;
            return (w * 0.5f + (cu / cw) * focal, h * 0.5f - (cv / cw) * focal);
        }

        if (ShowGrid)
        {
            var gridPen = new Pen(new SolidColorBrush(Color.FromRgb(80, 80, 90)), 0.5);
            var axisPen = new Pen(new SolidColorBrush(Color.FromRgb(120, 120, 130)), 0.8);
            int n = (int)(GridExtent / GridStep);
            for (int i = -n; i <= n; ++i)
            {
                float coord = i * GridStep;
                var p1 = Project(coord, 0, -GridExtent);
                var p2 = Project(coord, 0, GridExtent);
                if (p1.HasValue && p2.HasValue)
                {
                    var pen = (i == 0) ? axisPen : gridPen;
                    ctx.DrawLine(pen, new Point(p1.Value.sx, p1.Value.sy), new Point(p2.Value.sx, p2.Value.sy));
                }
                var p3 = Project(-GridExtent, 0, coord);
                var p4 = Project(GridExtent, 0, coord);
                if (p3.HasValue && p4.HasValue)
                {
                    var pen = (i == 0) ? axisPen : gridPen;
                    ctx.DrawLine(pen, new Point(p3.Value.sx, p3.Value.sy), new Point(p4.Value.sx, p4.Value.sy));
                }
            }
        }

        if (ShowAxes)
        {
            var xPen = new Pen(new SolidColorBrush(Color.FromRgb(220, 80, 80)), 1.5);
            var yPen = new Pen(new SolidColorBrush(Color.FromRgb(80, 220, 80)), 1.5);
            var zPen = new Pen(new SolidColorBrush(Color.FromRgb(80, 120, 220)), 1.5);
            var origin = Project(0, 0, 0);
            if (origin.HasValue)
            {
                var px = Project(2, 0, 0);
                var py = Project(0, 2, 0);
                var pz = Project(0, 0, 2);
                if (px.HasValue) ctx.DrawLine(xPen, new Point(origin.Value.sx, origin.Value.sy), new Point(px.Value.sx, px.Value.sy));
                if (py.HasValue) ctx.DrawLine(yPen, new Point(origin.Value.sx, origin.Value.sy), new Point(py.Value.sx, py.Value.sy));
                if (pz.HasValue) ctx.DrawLine(zPen, new Point(origin.Value.sx, origin.Value.sy), new Point(pz.Value.sx, pz.Value.sy));
            }
        }

        if (ShowGizmo)
        {
            // Gizmo is anchored at GizmoTarget in world space, projected to screen.
            var sp = Project(GizmoTarget.x, GizmoTarget.y, GizmoTarget.z);
            if (sp.HasValue)
            {
                float gx = sp.Value.sx, gy = sp.Value.sy;
                float len = MathF.Min(w, h) * 0.08f;
                var xPen = new Pen(new SolidColorBrush(Color.FromRgb(220, 80, 80)), 2);
                var yPen = new Pen(new SolidColorBrush(Color.FromRgb(80, 220, 80)), 2);
                var zPen = new Pen(new SolidColorBrush(Color.FromRgb(80, 120, 220)), 2);
                ctx.DrawLine(xPen, new Point(gx, gy), new Point(gx + len, gy));
                ctx.DrawLine(yPen, new Point(gx, gy), new Point(gx, gy - len));
                ctx.DrawLine(zPen, new Point(gx, gy), new Point(gx + len * 0.6f * ox, gy + len * 0.6f * oy));
            }
        }
    }
}
