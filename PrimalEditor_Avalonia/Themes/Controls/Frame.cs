using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.VisualTree;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// Attached properties that overlay a 9-slice bitmap frame on any control:
/// <code>
/// &lt;nodify:Node wc:Frame.Source="avares://.../frame-strip.png"
///              wc:Frame.Slice="7.5" wc:Frame.Scale="5" /&gt;
/// </code>
/// The frame is hosted by the nearest AdornerLayer (via
/// <see cref="AdornerLayer.SetAdorner"/>) and drawn over the control's
/// edges; the transparent center lets content show through. Set the source
/// to null (or simply don't set it — other skins) and nothing is attached.
/// Styles can set these attached properties per theme.
/// </summary>
public class Frame
{
    // 静态类不能作为 RegisterAttached 的 owner 类型参数,保留私有构造即可
    private Frame() { }

    public static readonly AttachedProperty<string?> SourceProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, string?>("Source");

    public static readonly AttachedProperty<double> SliceProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, double>("Slice", 8.0);

    public static readonly AttachedProperty<double> ScaleProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, double>("Scale", 2.0);

    /// <summary>Cache of the live adorner per decorated control.</summary>
    private static readonly AttachedProperty<NineSliceFrameAdorner?> AdornerProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, NineSliceFrameAdorner?>("__FrameAdorner");

    public static string? GetSource(Control control) => control.GetValue(SourceProperty);
    public static void SetSource(Control control, string? value) => control.SetValue(SourceProperty, value);
    public static double GetSlice(Control control) => control.GetValue(SliceProperty);
    public static void SetSlice(Control control, double value) => control.SetValue(SliceProperty, value);
    public static double GetScale(Control control) => control.GetValue(ScaleProperty);
    public static void SetScale(Control control, double value) => control.SetValue(ScaleProperty, value);

    static Frame()
    {
        SourceProperty.Changed.AddClassHandler<Control>(OnSourceChanged);
        SliceProperty.Changed.AddClassHandler<Control>(OnGeometryChanged);
        ScaleProperty.Changed.AddClassHandler<Control>(OnGeometryChanged);
    }

    private static void OnSourceChanged(Control control, AvaloniaPropertyChangedEventArgs e)
    {
        control.DetachedFromVisualTree -= OnDetached;
        control.AttachedToVisualTree -= OnAttached;
        control.AttachedToVisualTree += OnAttached;

        if (string.IsNullOrWhiteSpace(e.NewValue as string))
        {
            RemoveAdorner(control);
            return;
        }

        if (control.IsAttachedToVisualTree()) Attach(control);
    }

    private static void OnGeometryChanged(Control control, AvaloniaPropertyChangedEventArgs e)
    {
        if (control.GetValue(AdornerProperty) is { } adorner)
        {
            adorner.Slice = control.GetValue(SliceProperty);
            adorner.Scale = control.GetValue(ScaleProperty);
        }
    }

    private static void OnAttached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (sender is Control control) Attach(control);
    }

    private static void OnDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (sender is Control control) RemoveAdorner(control);
    }

    private static void Attach(Control control)
    {
        var source = control.GetValue(SourceProperty);
        if (string.IsNullOrWhiteSpace(source)) return;

        var adorner = new NineSliceFrameAdorner
        {
            Source = source,
            Slice = control.GetValue(SliceProperty),
            Scale = control.GetValue(ScaleProperty),
        };
        control.SetValue(AdornerProperty, adorner);
        // 受支持的挂载方式: AdornerLayer 自己找层、订阅元素 detach、按元素 bounds 排布
        AdornerLayer.SetAdorner(control, adorner);
    }

    private static void RemoveAdorner(Control control)
    {
        if (control.GetValue(AdornerProperty) is not { } adorner) return;
        AdornerLayer.SetAdorner(control, null);
        control.SetValue(AdornerProperty, null);
    }
}
