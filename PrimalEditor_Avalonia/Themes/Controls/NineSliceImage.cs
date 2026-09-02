using System;
using System.Collections.Concurrent;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// Draws one bitmap as a 9-slice (border-image): the four corner squares
/// render at fixed size, the four edges stretch along one axis and the
/// center stretches both — so a frame asset fits any control size without
/// corner distortion. Source is an avares URI; slices are in DIPs and
/// <see cref="Scale"/> converts them to source pixels.
/// </summary>
public class NineSliceImage : Control
{
    public static readonly StyledProperty<string?> SourceProperty =
        AvaloniaProperty.Register<NineSliceImage, string?>(nameof(Source));

    /// <summary>Corner/edge thickness in device-independent pixels.</summary>
    public static readonly StyledProperty<double> SliceProperty =
        AvaloniaProperty.Register<NineSliceImage, double>(nameof(Slice), 8.0);

    /// <summary>Source image pixels per DIP (2 = asset authored at 2x).</summary>
    public static readonly StyledProperty<double> ScaleProperty =
        AvaloniaProperty.Register<NineSliceImage, double>(nameof(Scale), 2.0);

    private static readonly ConcurrentDictionary<string, IImage?> SourceCache = new();

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

    static NineSliceImage()
    {
        AffectsRender<NineSliceImage>(SourceProperty, SliceProperty, ScaleProperty);
    }

    /// <summary>Resolve (and cache) the Source URI to a bitmap; null when unset or missing.</summary>
    public static IImage? LoadSource(string? source)
    {
        if (string.IsNullOrWhiteSpace(source)) return null;
        return SourceCache.GetOrAdd(source, static uri =>
        {
            try
            {
                var absolute = new Uri(uri, UriKind.Absolute);
                using var stream = AssetLoader.Open(absolute);
                return new Bitmap(stream);
            }
            catch (Exception ex)
            {
                Common.Logger.Warning($"NineSliceImage: failed to load '{uri}': {ex.Message}");
                return null;
            }
        });
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var slice = Slice;
        return new Size(slice * 2, slice * 2);
    }

    public override void Render(DrawingContext context)
    {
        var image = LoadSource(Source);
        if (image == null) return;
        DrawNineSlice(context, image, Bounds.Width, Bounds.Height, Slice, Scale);
    }

    /// <summary>Shared 9-slice painter (also used by TextureFrame).</summary>
    public static void DrawNineSlice(
        DrawingContext ctx, IImage image, double width, double height, double slice, double scale)
    {
        // Clamp slices so L+R (and T+B) never exceed the control — tiny
        // controls collapse the slices instead of inverting the rects.
        var s = Math.Max(0, slice);
        s = Math.Min(s, Math.Min(width, height) / 2);
        var src = s * scale;

        var iw = image.Size.Width;
        var ih = image.Size.Height;
        src = Math.Min(src, Math.Min(iw, ih) / 2);

        double dstL = src / scale, dstR = src / scale;
        double dstT = src / scale, dstB = src / scale;

        double cx = width - dstL - dstR;
        double cy = height - dstT - dstB;
        if (cx < 0) { dstL = Math.Min(dstL, width / 2); dstR = Math.Min(dstR, width - dstL); cx = 0; }
        if (cy < 0) { dstT = Math.Min(dstT, height / 2); dstB = Math.Min(dstB, height - dstT); cy = 0; }

        var scx = iw - src - src;
        var scy = ih - src - src;

        static Rect S(double x, double y, double w, double h) => new(x, y, Math.Max(w, 0), Math.Max(h, 0));
        static Rect D(double x, double y, double w, double h) => new(x, y, Math.Max(w, 0), Math.Max(h, 0));

        void Draw(Rect sRect, Rect dRect) => ctx.DrawImage(image, sRect, dRect);

        // corners (fixed size)
        Draw(S(0, 0, src, src), D(0, 0, dstL, dstT));
        Draw(S(iw - src, 0, src, src), D(width - dstR, 0, dstR, dstT));
        Draw(S(0, ih - src, src, src), D(0, height - dstB, dstL, dstB));
        Draw(S(iw - src, ih - src, src, src), D(width - dstR, height - dstB, dstR, dstB));

        // edges (one axis stretches)
        Draw(S(src, 0, scx, src), D(dstL, 0, cx, dstT));
        Draw(S(src, ih - src, scx, src), D(dstL, height - dstB, cx, dstB));
        Draw(S(0, src, src, scy), D(0, dstT, dstL, cy));
        Draw(S(iw - src, src, src, scy), D(width - dstR, dstT, dstR, cy));

        // center (both axes stretch)
        Draw(S(src, src, scx, scy), D(dstL, dstT, cx, cy));
    }
}
