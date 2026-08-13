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

    private (float tx, float ty, float tz) _target = (0, 0, 0);
    public void SetTarget(float x, float y, float z)
    {
        _target = (x, y, z);
        InvalidateVisual();
    }

    private (float yaw, float pitch, float distance, float vw, float vh) _camera;
    public void UpdateCamera(float yaw, float pitch, float distance, float vw, float vh)
    {
        _camera = (yaw, pitch, distance, vw, vh);
        InvalidateVisual();
    }

    private GizmoAxis _active = GizmoAxis.None;
    private Point _lastMouse;
    public bool IsDragging => _dragging;
    private bool _dragging;
    private IPointer? _capturedPointer;

    protected override Size MeasureOverride(Size availableSize) => availableSize;

    public override void Render(DrawingContext ctx)
    {
        var b = Bounds;
        if (b.Width <= 4 || b.Height <= 4) return;
        var (yaw, pitch, distance, vw, vh) = _camera;
        float focal = MathF.Min((float)b.Width, (float)b.Height) * 0.5f;

        WorldProjector.ComputeCameraBasis(
            0f, 0f, 0f, yaw, pitch, distance,
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
        var b = Bounds;
        if (b.Width <= 4) return null;
        var (yaw, pitch, distance, _, _) = _camera;
        float focal = MathF.Min((float)b.Width, (float)b.Height) * 0.5f;
        WorldProjector.ComputeCameraBasis(
            0f, 0f, 0f, yaw, pitch, distance,
            out var camX, out var camY, out var camZ,
            out var rx, out var ry, out var rz,
            out var ux, out var uy, out var uz,
            out var fx, out var fy, out var fz);
        return WorldProjector.Project(
            _target.tx, _target.ty, _target.tz,
            camX, camY, camZ,
            rx, ry, rz, ux, uy, uz, fx, fy, fz,
            (float)b.Width, (float)b.Height, focal);
    }

    private (float fx, float fy) GetForward()
    {
        var (_, pitch, distance, _, _) = _camera;
        var yaw = _camera.yaw;
        return (-MathF.Sin(yaw) * MathF.Cos(pitch), -MathF.Sin(pitch));
    }

    public bool TryStartDrag(Point p)
    {
        var center = GetGizmoScreenCenter();
        if (center == null) return false;
        float cx = center.Value.sx, cy = center.Value.sy;
        float len = MathF.Min((float)Bounds.Width, (float)Bounds.Height) * 0.12f;
        var (fx, fy) = GetForward();

        float dX = DistanceToSegment(p, new Point(cx, cy), new Point(cx + len, cy));
        float dY = DistanceToSegment(p, new Point(cx, cy), new Point(cx, cy - len));
        float dZ = DistanceToSegment(p, new Point(cx, cy), new Point(cx + len * 0.6f * fx, cy + len * 0.6f * fy));

        const float pickRadius = 14f;
        float best = MathF.Min(MathF.Min(dX, dY), dZ);
        if (best > pickRadius) return false;

        if (best == dX) _active = GizmoAxis.X;
        else if (best == dY) _active = GizmoAxis.Y;
        else _active = GizmoAxis.Z;

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

        float dist = _camera.distance;
        if (_mode == GizmoMode.Translate)
        {
            float delta = (_active == GizmoAxis.X) ? dx * dist * 0.01f : -dy * dist * 0.01f;
            OnDelta?.Invoke(this, (_mode, _active, delta));
        }
        else if (_mode == GizmoMode.Rotate)
        {
            float ang = (_active == GizmoAxis.X) ? dy * 0.01f : -dx * 0.01f;
            OnDelta?.Invoke(this, (_mode, _active, ang));
        }
        else
        {
            float ratio = 1f + ((_active == GizmoAxis.X) ? dx : -dy) * 0.01f;
            OnDelta?.Invoke(this, (_mode, _active, ratio));
        }
    }

    public void EndDrag()
    {
        _dragging = false;
        _active = GizmoAxis.None;
        InvalidateVisual();
    }
}
