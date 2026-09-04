using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace PrimalEditor_Avalonia.Editors.Particle;

/// <summary>
/// B4 P3: 自绘曲线编辑控件。渲染键帧折线 + 网格；交互：拖键帧（时间/值）、
/// 双击空白加键、右键删键、Ctrl+拖 float 键的切柄。任何编辑触发
/// <see cref="CurveEdited"/>，由宿主窗口负责推回引擎。
/// </summary>
public sealed class EmitterCurveEditor : Control
{
    public event Action? CurveEdited;

    public ParticleCurveSlot Slot { get; set; } = ParticleCurveSlot.Alpha;
    public ParticleCurveData? Curve { get; set; }

    // 交互状态
    private int _dragKey = -1;          // 被拖键帧索引；-1 无
    private bool _dragTangent;          // true=拖 out_tangent（Ctrl+拖）
    private const double HitRadiusPx = 10;

    private static readonly IBrush GridBrush = new SolidColorBrush(Color.FromRgb(60, 60, 70));
    private static readonly IBrush AxisBrush = new SolidColorBrush(Color.FromRgb(110, 110, 125));
    private static readonly IBrush CurveBrush = new SolidColorBrush(Color.FromRgb(0, 229, 255));
    private static readonly IBrush KeyBrush = new SolidColorBrush(Color.FromRgb(255, 200, 60));
    private static readonly IBrush TangentBrush = new SolidColorBrush(Color.FromRgb(255, 120, 120));
    private static readonly IPen CurvePen = new Pen(CurveBrush, 1.8);
    private static readonly IPen TangentPen = new Pen(TangentBrush, 1.0);

    private (float min, float max) ValueRange => ParticleCurveData.SlotValueRange(Slot);

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        Focusable = true;
        e.Pointer.Capture(this);
        var pt = e.GetCurrentPoint(this);
        var pos = pt.Position;
        int key = HitKey(pos);

        if (pt.Properties.IsRightButtonPressed && key >= 0)
        {
            RemoveKey(key);
            e.Handled = true;
            return;
        }

        if (pt.Properties.IsLeftButtonPressed)
        {
            if (key < 0 && e.ClickCount == 2)
            {
                key = AddKeyAt(pos);
                _dragKey = key;
                _dragTangent = false;
                e.Handled = true;
                return;
            }
            if (key >= 0)
            {
                _dragKey = key;
                _dragTangent = e.KeyModifiers.HasFlag(KeyModifiers.Control) && Curve?.HasTangents == true;
                e.Handled = true;
            }
        }
        InvalidateVisual();
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        if (_dragKey < 0 || Curve == null) return;
        var pos = e.GetPosition(this);
        var key = Curve.Keys[_dragKey];
        float t = PixelToTime(pos.X);
        float v = PixelToValue(pos.Y);

        if (_dragTangent)
        {
            // 切柄：tangent ≈ dValue/dTime（屏幕域换算到值域）
            float dt = t - key.Time;
            if (Math.Abs(dt) > 0.005f) key.OutTangent = (v - key.Value[0]) / dt;
        }
        else
        {
            key.Time = Math.Clamp(t, 0f, 1f);
            var (min, max) = ValueRange;
            key.Value[0] = Math.Clamp(v, min, max);
            for (int i = 1; i < key.Value.Length; ++i)
                key.Value[i] = Math.Clamp(key.Value[i], min, max);
            Curve.Keys.Sort((a, b) => a.Time.CompareTo(b.Time));
            _dragKey = Curve.Keys.IndexOf(key);
        }
        InvalidateVisual();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        if (_dragKey >= 0)
        {
            _dragKey = -1;
            _dragTangent = false;
            CurveEdited?.Invoke();
        }
        e.Pointer.Capture(null);
    }

    // ---- 键帧操作 ----------------------------------------------------------

    private int HitKey(Point pos)
    {
        if (Curve == null) return -1;
        double best = HitRadiusPx;
        int hit = -1;
        for (int i = 0; i < Curve.Keys.Count; ++i)
        {
            var p = KeyToPixel(Curve.Keys[i]);
            double d = (p.X - pos.X) * (p.X - pos.X) + (p.Y - pos.Y) * (p.Y - pos.Y);
            if (d < best * best) { best = Math.Sqrt(d); hit = i; }
        }
        return hit;
    }

    private int AddKeyAt(Point pos)
    {
        if (Curve == null) return -1;
        var (min, max) = ValueRange;
        var key = new ParticleCurveKey
        {
            Time = PixelToTime(pos.X),
            Value = Enumerable.Repeat(0f, Curve.ValueCount).ToArray(),
        };
        key.Value[0] = Math.Clamp(PixelToValue(pos.Y), min, max);
        Curve.Keys.Add(key);
        Curve.Keys.Sort((a, b) => a.Time.CompareTo(b.Time));
        CurveEdited?.Invoke();
        return Curve.Keys.IndexOf(key);
    }

    private void RemoveKey(int index)
    {
        if (Curve == null || index < 0 || index >= Curve.Keys.Count) return;
        Curve.Keys.RemoveAt(index);
        CurveEdited?.Invoke();
        InvalidateVisual();
    }

    // ---- 坐标换算 ----------------------------------------------------------

    private float PixelToTime(double x)
    {
        float w = (float)Math.Max(Bounds.Width, 1);
        return Math.Clamp((float)(x / w), 0f, 1f);
    }

    private float PixelToValue(double y)
    {
        float h = (float)Math.Max(Bounds.Height, 1);
        var (min, max) = ValueRange;
        return (float)(min + (1.0 - y / h) * (max - min));
    }

    private Point KeyToPixel(ParticleCurveKey k)
    {
        float w = (float)Math.Max(Bounds.Width, 1);
        float h = (float)Math.Max(Bounds.Height, 1);
        var (min, max) = ValueRange;
        float v = k.Value.Length > 0 ? k.Value[0] : 0f;
        float norm = (max - min) > 1e-6f ? (v - min) / (max - min) : 0f;
        return new Point(k.Time * w, (1f - Math.Clamp(norm, -0.2f, 1.2f)) * h);
    }

    // ---- 渲染 --------------------------------------------------------------

    public override void Render(DrawingContext ctx)
    {
        float w = (float)Math.Max(Bounds.Width, 1);
        float h = (float)Math.Max(Bounds.Height, 1);

        // 网格 + 中线
        var gridPen = new Pen(GridBrush, 0.5, dashStyle: new DashStyle(new double[] { 2, 3 }, 0));
        for (int i = 0; i <= 4; ++i)
        {
            float x = w * i / 4f;
            ctx.DrawLine(gridPen, new Point(x, 0), new Point(x, h));
            float y = h * i / 4f;
            ctx.DrawLine(gridPen, new Point(0, y), new Point(w, y));
        }
        ctx.DrawLine(new Pen(AxisBrush, 0.8), new Point(0, h * 0.5), new Point(w, h * 0.5));

        if (Curve == null) return;

        // 曲线：键间折线（分段线性可视——插值模式影响运行时求值，编辑器以
        // 折线近似 + 键位/切柄表达）。
        var keys = Curve.Keys;
        if (keys.Count == 0) return;

        var points = new List<Point>();
        if (keys[0].Time > 0f)
            points.Add(KeyToPixel(new ParticleCurveKey { Time = 0f, Value = (float[])keys[0].Value.Clone() }));
        foreach (var k in keys) points.Add(KeyToPixel(k));
        if (keys[^1].Time < 1f)
            points.Add(KeyToPixel(new ParticleCurveKey { Time = 1f, Value = (float[])keys[^1].Value.Clone() }));
        for (int i = 0; i + 1 < points.Count; ++i)
            ctx.DrawLine(CurvePen, points[i], points[i + 1]);

        // 键 + 切柄（float 槽位）
        var range = Math.Max(1e-6f, ValueRange.max - ValueRange.min);
        foreach (var k in keys)
        {
            var p = KeyToPixel(k);
            ctx.DrawEllipse(KeyBrush, null, p, 4.5, 4.5);
            if (Curve.HasTangents && Math.Abs(k.OutTangent) > 1e-4f)
            {
                const float dt = 0.08f;
                var tip = new Point(p.X + dt * w, p.Y - k.OutTangent * dt / range * h);
                ctx.DrawLine(TangentPen, p, tip);
                ctx.DrawEllipse(TangentBrush, null, tip, 3, 3);
            }
        }
    }
}
