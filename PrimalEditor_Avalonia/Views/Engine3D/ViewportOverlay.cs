using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using EntityTransform = PrimalEditor_Avalonia.Components.Transform;

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
    /// <summary>Scene whose Light and Camera helpers are shown.</summary>
    public Scene? Scene { get; set; }

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
        WorldProjector.ComputeCameraBasis(
            CameraTarget.x, CameraTarget.y, CameraTarget.z,
            CameraYaw, CameraPitch, CameraDistance,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);
        // Project a world-space point to screen coords.
        // Returns null if the point is behind the camera (z >= 0).
        (float sx, float sy)? Project(float px, float py, float pz)
        {
            float dx = px - camX, dy = py - camY, dz = pz - camZ;
            float cu = dx * rx + dy * ry + dz * rz;
            float cv = dx * ux + dy * uy + dz * uz;
            float cw = dx * fx + dy * fy + dz * fz;
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

        DrawSceneIcons(ctx, Project);

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
                ctx.DrawLine(zPen, new Point(gx, gy), new Point(gx + len * 0.6f * fx, gy + len * 0.6f * fy));
            }
        }
    }

    private void DrawSceneIcons(
        DrawingContext ctx,
        Func<float, float, float, (float sx, float sy)?> project)
    {
        if (Scene == null) return;

        var lightPen = new Pen(new SolidColorBrush(Color.FromRgb(255, 214, 64)), 2);
        var cameraPen = new Pen(new SolidColorBrush(Color.FromRgb(78, 205, 255)), 2);

        void Segment(EntityTransform t, Pen pen,
            (float x, float y, float z) a,
            (float x, float y, float z) b)
        {
            var wa = SceneIconGeometry.TransformPoint(t, a.x, a.y, a.z);
            var wb = SceneIconGeometry.TransformPoint(t, b.x, b.y, b.z);
            var sa = project(wa.x, wa.y, wa.z);
            var sb = project(wb.x, wb.y, wb.z);
            if (sa.HasValue && sb.HasValue)
                ctx.DrawLine(pen, new Point(sa.Value.sx, sa.Value.sy), new Point(sb.Value.sx, sb.Value.sy));
        }

        void DrawLight(EntityTransform t)
        {
            const int sides = 12;
            const float radius = 0.22f;
            const float rayInner = 0.31f;
            const float rayOuter = 0.48f;
            for (var i = 0; i < sides; i++)
            {
                var a = i * MathF.Tau / sides;
                var b = (i + 1) * MathF.Tau / sides;
                Segment(t, lightPen,
                    (MathF.Cos(a) * radius, MathF.Sin(a) * radius, 0),
                    (MathF.Cos(b) * radius, MathF.Sin(b) * radius, 0));
                if ((i & 1) == 0)
                {
                    Segment(t, lightPen,
                        (MathF.Cos(a) * rayInner, MathF.Sin(a) * rayInner, 0),
                        (MathF.Cos(a) * rayOuter, MathF.Sin(a) * rayOuter, 0));
                }
            }
        }

        void DrawCamera(EntityTransform t)
        {
            var near = new[]
            {
                (-0.28f, -0.20f, 0.10f), (0.28f, -0.20f, 0.10f),
                (0.28f, 0.20f, 0.10f), (-0.28f, 0.20f, 0.10f),
            };
            var far = new[]
            {
                (-0.48f, -0.34f, -0.72f), (0.48f, -0.34f, -0.72f),
                (0.48f, 0.34f, -0.72f), (-0.48f, 0.34f, -0.72f),
            };
            for (var i = 0; i < 4; i++)
            {
                Segment(t, cameraPen, near[i], near[(i + 1) % 4]);
                Segment(t, cameraPen, far[i], far[(i + 1) % 4]);
                Segment(t, cameraPen, near[i], far[i]);
            }
            Segment(t, cameraPen, (-0.16f, 0.20f, 0.10f), (0, 0.38f, 0.10f));
            Segment(t, cameraPen, (0, 0.38f, 0.10f), (0.16f, 0.20f, 0.10f));
        }

        void Visit(GameEntity entity)
        {
            if (!entity.IsEnabled) return;
            var transform = entity.GetTransform();
            if (transform != null)
            {
                if (entity.GetComponent<LightComponent>() != null) DrawLight(transform);
                if (entity.GetComponent<CameraComponent>() != null) DrawCamera(transform);
            }
            foreach (var child in entity.Children) Visit(child);
        }

        foreach (var entity in Scene.Entities) Visit(entity);
    }
}
