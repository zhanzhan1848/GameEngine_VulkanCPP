using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public partial class GeometryEditorView : UserControl
{
    private float _yaw, _pitch, _distance = 12f;
    private float[] _target = { 0f, 0f, 0f };
    private bool _drag;
    private Avalonia.Point _last;
    private Panel? _panel;

    public GeometryEditorView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Render();
        _panel = this.FindControl<Panel>("RenderPanel");
        if (_panel != null) _panel.LayoutUpdated += (_, _) => Render();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        _drag = true;
        _last = e.GetPosition(_panel);
        e.Pointer.Capture(_panel);
        e.Handled = true;
    }

    private void OnPointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (!_drag || _panel == null) return;
        var p = e.GetPosition(_panel);
        var dx = (float)(p.X - _last.X);
        var dy = (float)(p.Y - _last.Y);
        _last = p;
        _yaw -= dx * 0.01f;
        _pitch -= dy * 0.01f;
        if (_pitch > 1.4f) _pitch = 1.4f;
        if (_pitch < -1.4f) _pitch = -1.4f;
        Render();
        e.Handled = true;
    }

    private void OnPointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        _drag = false;
        e.Pointer.Capture(null);
    }

    private void Render()
    {
        if (_panel == null) return;
        if (DataContext is not GeometryEditorViewModel vm) return;
        if (vm.SelectedLOD == null || vm.SelectedLOD.Meshes.Count == 0)
        {
            DrawPlaceholder();
            return;
        }
        var mesh = vm.SelectedLOD.Meshes[0];
        var positions = mesh.GetPositionsAsFloat3();
        var indices = mesh.GetIndicesAsInt();
        if (positions.Length == 0)
        {
            DrawPlaceholder();
            return;
        }
        var center = ComputeCenter(positions);
        var size = ComputeSize(positions);
        DrawWireframe(positions, indices, center, size);
    }

    private void DrawPlaceholder()
    {
        if (_panel == null) return;
        _panel.Children.Clear();
        var t = new TextBlock
        {
            Text = "No geometry loaded",
            Foreground = new SolidColorBrush(Color.FromRgb(160, 160, 160)),
            FontSize = 14,
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center,
            VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
        };
        _panel.Children.Add(t);
    }

    private (float x, float y, float z) ComputeCenter(float[] positions)
    {
        float sx = 0, sy = 0, sz = 0;
        int n = positions.Length / 3;
        for (int i = 0; i < n; i++)
        {
            sx += positions[i * 3 + 0];
            sy += positions[i * 3 + 1];
            sz += positions[i * 3 + 2];
        }
        return n == 0 ? (0, 0, 0) : (sx / n, sy / n, sz / n);
    }

    private float ComputeSize(float[] positions)
    {
        float max = 0;
        for (int i = 0; i < positions.Length; i++)
            if (Math.Abs(positions[i]) > max) max = Math.Abs(positions[i]);
        return max == 0 ? 1f : max;
    }

    private void DrawWireframe(float[] positions, int[] indices,
        (float x, float y, float z) center, float size)
    {
        if (_panel == null) return;
        _panel.Children.Clear();

        var w = _panel.Bounds.Width;
        var h = _panel.Bounds.Height;
        if (w <= 0 || h <= 0) return;

        var cy = MathF.Cos(_yaw);
        var sy = MathF.Sin(_yaw);
        var cp = MathF.Cos(_pitch);
        var sp = MathF.Sin(_pitch);
        var scale = (float)(Math.Min(w, h) / (size * 2.5f + 0.001f));

        var projected = new (float x, float y)[positions.Length / 3];
        for (int i = 0; i < projected.Length; i++)
        {
            var px = positions[i * 3 + 0] - center.x;
            var py = positions[i * 3 + 1] - center.y;
            var pz = positions[i * 3 + 2] - center.z;
            var rx = px * cy - pz * sy;
            var rz = px * sy + pz * cy;
            var ry = py * cp - rz * sp;
            rz = py * sp + rz * cp;
            projected[i] = ((float)((rx * scale) + w / 2.0), (float)((ry * scale) + h / 2.0));
        }

        var brush = new SolidColorBrush(Color.FromRgb(120, 220, 120));
        var triangles = indices.Length / 3;
        for (int t = 0; t < triangles; t++)
        {
            var i0 = indices[t * 3 + 0];
            var i1 = indices[t * 3 + 1];
            var i2 = indices[t * 3 + 2];
            if (i0 >= projected.Length || i1 >= projected.Length || i2 >= projected.Length) continue;
            AddLine(projected[i0], projected[i1], brush);
            AddLine(projected[i1], projected[i2], brush);
            AddLine(projected[i2], projected[i0], brush);
        }
    }

    private void AddLine((float x, float y) a, (float x, float y) b, IBrush brush)
    {
        var line = new Avalonia.Controls.Shapes.Line
        {
            StartPoint = new Avalonia.Point(a.x, a.y),
            EndPoint = new Avalonia.Point(b.x, b.y),
            Stroke = brush,
            StrokeThickness = 0.7,
        };
        _panel!.Children.Add(line);
    }
}