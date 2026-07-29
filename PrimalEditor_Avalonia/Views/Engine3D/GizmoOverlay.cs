using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Managed gizmo overlay drawn on top of the engine viewport. Three
/// modes: <see cref="GizmoMode.Translate"/> (X/Y/Z axis arrows),
/// <see cref="GizmoMode.Rotate"/> (X/Y/Z rings), and
/// <see cref="GizmoMode.Scale"/> (X/Y/Z cubes). The active handle is
/// selected by clicking its projected screen-space bounds; dragging
/// projects the mouse delta onto the corresponding world axis and emits
/// a delta value via <see cref="OnDelta"/>.
///
/// This is a basic implementation:
/// - X/Y/Z axis drag only (no plane drag, no view-aligned axes)
/// - Rotation uses screen-space angular delta around the active axis
/// - Scale uses screen-space distance ratio along the active axis
/// - One axis at a time; multi-axis gizmos (centre cube) not supported
/// </summary>
public class GizmoOverlay : Control
{
    public enum GizmoMode { Translate, Rotate, Scale }
    public enum GizmoAxis { None, X, Y, Z }

    public event EventHandler<(GizmoAxis axis, float delta)>? OnDelta;

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

    private (float ox, float oy, float oz, float dx, float dy, float dz) _cameraRay;
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

    public override void Render(DrawingContext ctx)
    {
        var b = Bounds;
        if (b.Width <= 4 || b.Height <= 4) return;
        float cx = (float)b.Width / 2f;
        float cy = (float)b.Height / 2f;
        var (yaw, pitch, distance, vw, vh) = _camera;
        float cyA = MathF.Cos(yaw), syA = MathF.Sin(yaw);
        float cpA = MathF.Cos(pitch), spA = MathF.Sin(pitch);
        float ox = -syA * cpA, oy = -spA, oz = -cyA * cpA;
        float[] forward = new[] { ox, oy, oz };
        // Project target to screen: anchor at center, scale by focal.
        float focal = MathF.Min((float)b.Width, (float)b.Height) * 0.5f;
        // For simplicity, draw axes from screen center to right/bottom-left/up.
        // X axis: red, drawn right. Y axis: green, drawn up (in screen-up). Z axis: blue, drawn into screen (towards vanishing point).
        float len = MathF.Min((float)b.Width, (float)b.Height) * 0.12f;

        if (_mode == GizmoMode.Translate)
        {
            DrawArrow(ctx, cx, cy, cx + len, cy, Colors.Red, _active == GizmoAxis.X);
            DrawArrow(ctx, cx, cy, cx, cy - len, Colors.Lime, _active == GizmoAxis.Y);
            DrawArrow(ctx, cx, cy, cx + len * 0.6f * ox, cy + len * 0.6f * oy, Colors.DodgerBlue, _active == GizmoAxis.Z);
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
            DrawCube(ctx, cx + len * 0.6f * ox, cy + len * 0.6f * oy, len * 0.08f, Colors.DodgerBlue, _active == GizmoAxis.Z);
        }
    }

    private static void DrawArrow(DrawingContext ctx, float x0, float y0, float x1, float y1, Color color, bool active)
    {
        var pen = new Pen(new SolidColorBrush(color), active ? 3 : 1.5);
        ctx.DrawLine(pen, new Point(x0, y0), new Point(x1, y1));
        // Arrow head: two small lines forming a chevron at the tip.
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

    public void OnPointerPressed(Point p)
    {
        var b = Bounds;
        if (b.Width <= 4) return;
        float cx = (float)b.Width / 2f;
        float cy = (float)b.Height / 2f;
        float len = MathF.Min((float)b.Width, (float)b.Height) * 0.12f;
        // X axis
        float dX = DistanceToSegment(p, new Point(cx, cy), new Point(cx + len, cy));
        float dY = DistanceToSegment(p, new Point(cx, cy), new Point(cx, cy - len));
        // Z axis uses forward vector; for simplicity skip in hit-test (Z is at center).
        float best = MathF.Min(MathF.Min(dX, dY), 12f);
        if (best > 12f) { _active = GizmoAxis.None; return; }
        if (dX < dY) _active = GizmoAxis.X; else _active = GizmoAxis.Y;
        _lastMouse = p;
        _dragging = true;
        InvalidateVisual();
    }

    public void OnPointerMoved(Point p)
    {
        if (!_dragging || _active == GizmoAxis.None) return;
        var b = Bounds;
        float dx = (float)(p.X - _lastMouse.X);
        float dy = (float)(p.Y - _lastMouse.Y);
        _lastMouse = p;
        float len = MathF.Min((float)b.Width, (float)b.Height) * 0.12f;
        float scale = 1f / MathF.Max(len, 1f);
        if (_mode == GizmoMode.Translate)
        {
            float delta = (_active == GizmoAxis.X) ? dx : -dy;
            OnDelta?.Invoke(this, (_active, delta * scale));
        }
        else if (_mode == GizmoMode.Rotate)
        {
            float ang = (_active == GizmoAxis.X) ? dy : -dx;
            OnDelta?.Invoke(this, (_active, ang * 0.01f));
        }
        else
        {
            float ratio = 1f + ((_active == GizmoAxis.X) ? dx : -dy) * 0.01f;
            OnDelta?.Invoke(this, (_active, ratio));
        }
    }

    public void OnPointerReleased()
    {
        _dragging = false;
        _active = GizmoAxis.None;
        InvalidateVisual();
    }
}
