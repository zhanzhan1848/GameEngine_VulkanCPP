using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace PrimalEditor_Avalonia.Views.Engine3D;

public class GizmoOverlay : Control
{
    public enum GizmoMode { Translate, Rotate, Scale }
    public enum GizmoAxis { None, X, Y, Z }

    public event EventHandler<(GizmoMode mode, GizmoAxis axis, float delta)>? OnDelta;

    private GizmoMode _mode = GizmoMode.Translate;
    public GizmoMode Mode
    {
        get => _mode;
        set { if (_mode != value) { _mode = value; InvalidateVisual(); } }
    }

    public bool IsVisibleInViewport { get; set; }

    private (float tx, float ty, float tz) _target = (0, 0, 0);
    public void SetTarget(float x, float y, float z)
    {
        _target = (x, y, z);
        InvalidateVisual();
    }

    private (float yaw, float pitch, float distance, float targetX, float targetY, float targetZ, float vw, float vh) _camera;
    public void UpdateCamera(float yaw, float pitch, float distance,
        float targetX, float targetY, float targetZ, float vw, float vh)
    {
        _camera = (yaw, pitch, distance, targetX, targetY, targetZ, vw, vh);
        InvalidateVisual();
    }

    private GizmoAxis _active = GizmoAxis.None;
    private Point _lastMouse;
    private (float x, float y) _activeScreenDirection = (1f, 0f);
    public bool IsDragging => _dragging;
    private bool _dragging;
    protected override Size MeasureOverride(Size availableSize) => availableSize;

    public override void Render(DrawingContext ctx)
    {
        if (!IsVisibleInViewport) return;
        var b = Bounds;
        if (b.Width <= 4 || b.Height <= 4) return;
        var (yaw, pitch, distance, targetX, targetY, targetZ, vw, vh) = _camera;
        float focal = WorldProjector.PerspectiveFocalLength((float)b.Height);

        WorldProjector.ComputeCameraBasis(
            targetX, targetY, targetZ, yaw, pitch, distance,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);

        var sp = WorldProjector.Project(
            _target.tx, _target.ty, _target.tz,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            (float)b.Width, (float)b.Height, focal);
        if (!sp.HasValue) return;

        float cx = sp.Value.sx;
        float cy = sp.Value.sy;
        float len = MathF.Min((float)b.Width, (float)b.Height) * 0.12f;

        if (_mode == GizmoMode.Translate)
        {
            DrawArrow(ctx, cx, cy, cx + len, cy, Colors.Red, _active == GizmoAxis.X);
            DrawArrow(ctx, cx, cy, cx, cy - len, Colors.Lime, _active == GizmoAxis.Y);
            DrawArrow(ctx, cx, cy, cx + len * 0.6f * fx, cy + len * 0.6f * fy, Colors.DodgerBlue, _active == GizmoAxis.Z);
        }
        else if (_mode == GizmoMode.Rotate)
        {
            DrawRing(ctx, cx, cy, len * 0.7f, Colors.Red, _active == GizmoAxis.X);
            DrawRing(ctx, cx, cy, len * 0.7f, Colors.Lime, _active == GizmoAxis.Y);
            DrawRing(ctx, cx, cy, len * 0.7f, Colors.DodgerBlue, _active == GizmoAxis.Z);
        }
        else
        {
            DrawCube(ctx, cx + len, cy, len * 0.08f, Colors.Red, _active == GizmoAxis.X);
            DrawCube(ctx, cx, cy - len, len * 0.08f, Colors.Lime, _active == GizmoAxis.Y);
            DrawCube(ctx, cx + len * 0.6f * fx, cy + len * 0.6f * fy, len * 0.08f, Colors.DodgerBlue, _active == GizmoAxis.Z);
        }
    }

    private static void DrawArrow(DrawingContext ctx, float x0, float y0, float x1, float y1, Color color, bool active)
    {
        var pen = new Pen(new SolidColorBrush(color), active ? 3 : 1.5);
        ctx.DrawLine(pen, new Point(x0, y0), new Point(x1, y1));
        var dx = x1 - x0; var dy = y1 - y0;
        float len = MathF.Sqrt(dx * dx + dy * dy);
        if (len < 4) return;
        float ux = dx / len, uy = dy / len;
        float hx = -uy * 0.5f, hy = ux * 0.5f;
        var headPen = new Pen(new SolidColorBrush(color), active ? 3 : 2);
        ctx.DrawLine(headPen, new Point(x1, y1), new Point(x1 - ux * 8 + hx, y1 - uy * 8 + hy));
        ctx.DrawLine(headPen, new Point(x1, y1), new Point(x1 - ux * 8 - hx, y1 - uy * 8 - hy));
    }

    private static void DrawRing(DrawingContext ctx, float cx, float cy, float r, Color color, bool active)
    {
        var fill = new SolidColorBrush(color, active ? 0.2f : 0.1f);
        var pen = new Pen(new SolidColorBrush(color), active ? 3 : 1.5);
        ctx.DrawEllipse(fill, pen, new Rect(cx - r, cy - r, r * 2, r * 2));
    }

    private static void DrawCube(DrawingContext ctx, float cx, float cy, float r, Color color, bool active)
    {
        var fill = new SolidColorBrush(color, active ? 0.9 : 0.6);
        var pen = new Pen(Brushes.White, 0.5);
        ctx.DrawRectangle(fill, pen, new Rect(cx - r, cy - r, r * 2, r * 2));
    }

    private static float DistanceToSegment(Point p, Point a, Point b)
    {
        float dx = (float)(b.X - a.X), dy = (float)(b.Y - a.Y);
        float len2 = dx * dx + dy * dy;
        if (len2 < 1e-3f) return (float)Math.Sqrt((p.X - a.X) * (p.X - a.X) + (p.Y - a.Y) * (p.Y - a.Y));
        float t = MathF.Max(0, MathF.Min(1, ((float)(p.X - a.X) * dx + (float)(p.Y - a.Y) * dy) / len2));
        float px = (float)a.X + t * dx, py = (float)a.Y + t * dy;
        float ex = (float)p.X - px, ey = (float)p.Y - py;
        return MathF.Sqrt(ex * ex + ey * ey);
    }

    private (float sx, float sy)? GetGizmoScreenCenter()
    {
        return ProjectWorld(_target.tx, _target.ty, _target.tz);
    }

    private (float sx, float sy)? ProjectWorld(float x, float y, float z)
    {
        var b = Bounds;
        if (b.Width <= 4 || b.Height <= 4) return null;
        var (yaw, pitch, distance, targetX, targetY, targetZ, _, _) = _camera;
        float focal = WorldProjector.PerspectiveFocalLength((float)b.Height);
        WorldProjector.ComputeCameraBasis(
            targetX, targetY, targetZ, yaw, pitch, distance,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);
        return WorldProjector.Project(
            x, y, z,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            (float)b.Width, (float)b.Height, focal);
    }

    private (float fx, float fy) GetForward()
    {
        var (_, pitch, distance, _, _, _, _, _) = _camera;
        var yaw = _camera.yaw;
        return (MathF.Sin(yaw) * MathF.Cos(pitch), MathF.Sin(pitch));
    }

    public bool TryStartDrag(Point p)
    {
        if (!IsVisibleInViewport) return false;
        var center = GetGizmoScreenCenter();
        if (center == null) return false;
        float cx = center.Value.sx, cy = center.Value.sy;
        float worldLen = Math.Clamp(_camera.distance * 0.12f, 0.65f, 4f);

        float RingDistance(int plane)
        {
            const int segments = 48;
            var best = float.MaxValue;
            (float sx, float sy)? previous = null;
            for (var i = 0; i <= segments; ++i)
            {
                var a = MathF.Tau * i / segments;
                var world = plane switch
                {
                    0 => (_target.tx, _target.ty + MathF.Cos(a) * worldLen, _target.tz + MathF.Sin(a) * worldLen),
                    1 => (_target.tx + MathF.Cos(a) * worldLen, _target.ty, _target.tz + MathF.Sin(a) * worldLen),
                    _ => (_target.tx + MathF.Cos(a) * worldLen, _target.ty + MathF.Sin(a) * worldLen, _target.tz),
                };
                var current = ProjectWorld(world.Item1, world.Item2, world.Item3);
                if (previous.HasValue && current.HasValue)
                {
                    best = MathF.Min(best, DistanceToSegment(
                        p,
                        new Point(previous.Value.sx, previous.Value.sy),
                        new Point(current.Value.sx, current.Value.sy)));
                }
                previous = current;
            }
            return best;
        }

        float dX, dY, dZ;
        if (_mode == GizmoMode.Rotate)
        {
            dX = RingDistance(0);
            dY = RingDistance(1);
            dZ = RingDistance(2);
        }
        else
        {
            var xEnd = ProjectWorld(_target.tx + worldLen, _target.ty, _target.tz);
            var yEnd = ProjectWorld(_target.tx, _target.ty + worldLen, _target.tz);
            var zEnd = ProjectWorld(_target.tx, _target.ty, _target.tz + worldLen);
            if (!xEnd.HasValue || !yEnd.HasValue || !zEnd.HasValue) return false;
            var origin = new Point(cx, cy);
            dX = DistanceToSegment(p, origin, new Point(xEnd.Value.sx, xEnd.Value.sy));
            dY = DistanceToSegment(p, origin, new Point(yEnd.Value.sx, yEnd.Value.sy));
            dZ = DistanceToSegment(p, origin, new Point(zEnd.Value.sx, zEnd.Value.sy));
        }

        const float pickRadius = 14f;
        float best = MathF.Min(MathF.Min(dX, dY), dZ);
        if (best > pickRadius) return false;

        if (best == dX) _active = GizmoAxis.X;
        else if (best == dY) _active = GizmoAxis.Y;
        else _active = GizmoAxis.Z;

        if (_mode != GizmoMode.Rotate)
        {
            var axisEnd = _active switch
            {
                GizmoAxis.X => ProjectWorld(_target.tx + worldLen, _target.ty, _target.tz),
                GizmoAxis.Y => ProjectWorld(_target.tx, _target.ty + worldLen, _target.tz),
                _ => ProjectWorld(_target.tx, _target.ty, _target.tz + worldLen),
            };
            if (axisEnd.HasValue)
            {
                var dirX = axisEnd.Value.sx - cx;
                var dirY = axisEnd.Value.sy - cy;
                var dirLength = MathF.Sqrt(dirX * dirX + dirY * dirY);
                if (dirLength > 1e-4f)
                    _activeScreenDirection = (dirX / dirLength, dirY / dirLength);
            }
        }

        _lastMouse = p;
        _dragging = true;
        InvalidateVisual();
        return true;
    }

    public void DragDelta(Point p)
    {
        if (!_dragging || _active == GizmoAxis.None) return;
        float dx = (float)(p.X - _lastMouse.X);
        float dy = (float)(p.Y - _lastMouse.Y);
        _lastMouse = p;
        float alongAxis = dx * _activeScreenDirection.x + dy * _activeScreenDirection.y;

        float dist = _camera.distance;
        if (_mode == GizmoMode.Translate)
        {
            var focal = WorldProjector.PerspectiveFocalLength((float)Bounds.Height);
            float delta = alongAxis * dist / MathF.Max(focal, 1f);
            OnDelta?.Invoke(this, (_mode, _active, delta));
        }
        else if (_mode == GizmoMode.Rotate)
        {
            float ang = (_active == GizmoAxis.X) ? dy * 0.01f : -dx * 0.01f;
            OnDelta?.Invoke(this, (_mode, _active, ang));
        }
        else
        {
            float ratio = 1f + alongAxis * 0.01f;
            OnDelta?.Invoke(this, (_mode, _active, ratio));
        }
    }

    public void EndDrag()
    {
        _dragging = false;
        _active = GizmoAxis.None;
        _activeScreenDirection = (1f, 0f);
        InvalidateVisual();
    }
}
