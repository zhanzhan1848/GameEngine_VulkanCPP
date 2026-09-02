using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// A Decorator whose chrome comes from a 9-slice bitmap when
/// <see cref="Source"/> is set (Warcraft-style frame art). When Source is
/// unset (themes that ship no art, e.g. Cyberpunk) it paints a plain
/// Background/BorderBrush/CornerRadius rectangle instead — visually a
/// Border — so views can host it without caring which theme is active.
/// The child content always draws on top of the frame.
/// </summary>
public class TextureFrame : Decorator
{
    public static readonly StyledProperty<string?> SourceProperty =
        AvaloniaProperty.Register<TextureFrame, string?>(nameof(Source));

    public static readonly StyledProperty<double> SliceProperty =
        AvaloniaProperty.Register<TextureFrame, double>(nameof(Slice), 8.0);

    public static readonly StyledProperty<double> ScaleProperty =
        AvaloniaProperty.Register<TextureFrame, double>(nameof(Scale), 2.0);

    public static readonly StyledProperty<IBrush?> BackgroundProperty =
        AvaloniaProperty.Register<TextureFrame, IBrush?>(nameof(Background));

    public static readonly StyledProperty<IBrush?> BorderBrushProperty =
        AvaloniaProperty.Register<TextureFrame, IBrush?>(nameof(BorderBrush));

    public static readonly StyledProperty<double> BorderThicknessProperty =
        AvaloniaProperty.Register<TextureFrame, double>(nameof(BorderThickness));

    public static readonly StyledProperty<CornerRadius> CornerRadiusProperty =
        AvaloniaProperty.Register<TextureFrame, CornerRadius>(nameof(CornerRadius));

    public static readonly StyledProperty<BoxShadows> BoxShadowProperty =
        AvaloniaProperty.Register<TextureFrame, BoxShadows>(nameof(BoxShadow));

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

    public IBrush? Background
    {
        get => GetValue(BackgroundProperty);
        set => SetValue(BackgroundProperty, value);
    }

    public IBrush? BorderBrush
    {
        get => GetValue(BorderBrushProperty);
        set => SetValue(BorderBrushProperty, value);
    }

    public double BorderThickness
    {
        get => GetValue(BorderThicknessProperty);
        set => SetValue(BorderThicknessProperty, value);
    }

    public CornerRadius CornerRadius
    {
        get => GetValue(CornerRadiusProperty);
        set => SetValue(CornerRadiusProperty, value);
    }

    /// <summary>Drop shadow for the fallback (no art) rendering; ignored when
    /// 9-slice art is active since frame art carries its own depth.</summary>
    public BoxShadows BoxShadow
    {
        get => GetValue(BoxShadowProperty);
        set => SetValue(BoxShadowProperty, value);
    }

    static TextureFrame()
    {
        AffectsRender<TextureFrame>(SourceProperty, SliceProperty, ScaleProperty,
            BackgroundProperty, BorderBrushProperty, BorderThicknessProperty, CornerRadiusProperty);
        AffectsArrange<TextureFrame>(BoxShadowProperty);
    }

    public override void Render(DrawingContext context)
    {
        var image = NineSliceImage.LoadSource(Source);
        if (image == null)
        {
            PaintFallbackBorder(context, Bounds.Width, Bounds.Height);
            return;
        }
        NineSliceImage.DrawNineSlice(context, image, Bounds.Width, Bounds.Height, Slice, Scale);
    }

    /// <summary>Solid Background + uniform BorderBrush chrome, Border-style.</summary>
    private void PaintFallbackBorder(DrawingContext context, double width, double height)
    {
        var thickness = BorderThickness;
        var radius = CornerRadius.TopLeft;
        if (Background != null)
        {
            var fillRect = new Rect(0, 0, width, height).Deflate(thickness / 2);
            context.DrawRectangle(Background, null, new RoundedRect(fillRect, new CornerRadius(radius)), BoxShadow);
        }
        if (BorderBrush != null && thickness > 0)
        {
            var strokeRect = new Rect(0, 0, width, height).Deflate(thickness / 2);
            context.DrawRectangle(null, new Pen(BorderBrush, thickness), new RoundedRect(strokeRect, new CornerRadius(radius)), BoxShadow);
        }
    }
}
