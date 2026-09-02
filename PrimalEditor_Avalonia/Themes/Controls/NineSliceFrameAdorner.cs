using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using AdornerLayer = Avalonia.Controls.Primitives.AdornerLayer;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// Adorner that paints a 9-slice bitmap frame AROUND the adorned element:
/// the ring is drawn outside the element's bounds (overhang = Slice), so it
/// never covers the element's own content. Render-only: hit testing and
/// layout are unaffected.
/// </summary>
public class NineSliceFrameAdorner : Control
{
    public static readonly StyledProperty<string?> SourceProperty =
        AvaloniaProperty.Register<NineSliceFrameAdorner, string?>(nameof(Source));

    public static readonly StyledProperty<double> SliceProperty =
        AvaloniaProperty.Register<NineSliceFrameAdorner, double>(nameof(Slice), 8.0);

    public static readonly StyledProperty<double> ScaleProperty =
        AvaloniaProperty.Register<NineSliceFrameAdorner, double>(nameof(Scale), 2.0);

    public string? Source
    {
        get => GetValue(SourceProperty);
        set => SetValue(SourceProperty, value);
    }

    public double Slice
    {
        get => GetValue(SliceProperty);
        set => SetValue(SliceProperty, value);
    }

    public double Scale
    {
        get => GetValue(ScaleProperty);
        set => SetValue(ScaleProperty, value);
    }

    static NineSliceFrameAdorner()
    {
        AffectsRender<NineSliceFrameAdorner>(SourceProperty, SliceProperty, ScaleProperty);
        IsHitTestVisibleProperty.OverrideDefaultValue<NineSliceFrameAdorner>(false);
        // 允许框环外溢到节点边界之外(否则会被裁掉,变成挤压内容)
        AdornerLayer.IsClipEnabledProperty.OverrideDefaultValue<NineSliceFrameAdorner>(false);
    }

    public override void Render(DrawingContext context)
    {
        var image = NineSliceImage.LoadSource(Source);
        if (image == null) return;
        // 外扩 Slice: 框环完全落在元素边界之外,不挤压内容
        using var _ = context.PushTransform(Matrix.CreateTranslation(-Slice, -Slice));
        NineSliceImage.DrawNineSlice(context, image,
            Bounds.Width + Slice * 2, Bounds.Height + Slice * 2, Slice, Scale);
    }
}
