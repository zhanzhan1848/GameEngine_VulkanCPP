using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// ContentControl host that paints its content inside a chamfered
/// (top-right + bottom-left cut) panel with hairline neon strokes along
/// the cuts. Visual signature of the cyberpunk deck theme.
/// </summary>
public class CyberCorner : ContentControl
{
    /// <summary>How far the chamfer cuts in, in device-independent pixels.</summary>
    public static readonly StyledProperty<double> CornerSizeProperty =
        AvaloniaProperty.Register<CyberCorner, double>(nameof(CornerSize), 14.0);

    /// <summary>Glow color for the hairline strokes along the cuts.</summary>
    public static readonly StyledProperty<Color> GlowColorProperty =
        AvaloniaProperty.Register<CyberCorner, Color>(nameof(GlowColor), Color.FromRgb(0x00, 0xF0, 0xFF));

    /// <summary>Fill color of the panel.</summary>
    public static readonly StyledProperty<Color> FillColorProperty =
        AvaloniaProperty.Register<CyberCorner, Color>(nameof(FillColor), Color.FromRgb(0x13, 0x13, 0x1F));

    public double CornerSize
    {
        get => GetValue(CornerSizeProperty);
        set => SetValue(CornerSizeProperty, value);
    }

    public Color GlowColor
    {
        get => GetValue(GlowColorProperty);
        set => SetValue(GlowColorProperty, value);
    }

    public Color FillColor
    {
        get => GetValue(FillColorProperty);
        set => SetValue(FillColorProperty, value);
    }
}
