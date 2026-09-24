using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Converters;

public static class FileViewModeConverters
{
    public static readonly IValueConverter ToThumbnails = new ViewModeToBoolConverter { Target = FileViewMode.Thumbnails };
    public static readonly IValueConverter IsList = new ViewModeToBoolConverter { Target = FileViewMode.List };
    public static readonly IValueConverter IsThumbnails = new ViewModeToBoolConverter { Target = FileViewMode.Thumbnails };
}

public sealed class BoolInverterConverter : IValueConverter
{
    public static readonly BoolInverterConverter Instance = new();
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b ? !b : (object?)false;
    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b ? !b : (object?)false;
}

public static class CommonConverters
{
    public static readonly IValueConverter InvertBool = BoolInverterConverter.Instance;
    public static readonly IValueConverter BoolToOnOffText = new BoolToOnOffConverter();
    public static readonly IValueConverter BoolToOnOffBrush = new BoolToOnOffBrushConverter();
}

/// <summary>Maps a bool to "ON" / "OFF" display text for module-status badges.</summary>
public sealed class BoolToOnOffConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b && b ? "ON" : "OFF";
    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

/// <summary>
/// Maps a bool to an accent brush suitable for the ON/OFF badge background:
/// electric blue (active) vs muted hairline (disabled).
/// </summary>
public sealed class BoolToOnOffBrushConverter : IValueConverter
{
    private static readonly IBrush OnBrush = new SolidColorBrush(Color.FromArgb(0x66, 0x6F, 0xA8, 0xFF));  // ElectricBlue @ 40%
    private static readonly IBrush OffBrush = new SolidColorBrush(Color.FromArgb(0x66, 0x2C, 0x33, 0x49)); // Hairline   @ 40%

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b && b ? OnBrush : (IBrush)OffBrush;
    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

public class ViewModeToBoolConverter : IValueConverter
{
    public FileViewMode Target { get; set; }
    public bool Inverted { get; set; }

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        var match = value is FileViewMode m && m == Target;
        return Inverted ? !match : match;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool b && b) return Target;
        return FileViewMode.List;
    }
}

public static class FileTypeConverters
{
    public static readonly IValueConverter ToIcon = new FileTypeToIconConverter();
    public static readonly IValueConverter ToThumbnailSource = new AssetThumbnailConverter();
}

public class AssetThumbnailConverter : IValueConverter
{
    private static readonly ConcurrentDictionary<string, WeakReference<Avalonia.Media.Imaging.Bitmap>> _cache = new();

    /// <summary>
    /// Returns a renderable source for an asset thumbnail. For engine
    /// textures (.texture/TEXR) this decodes mip-0 via
    /// <see cref="TextureTexrReader"/> (cached per path); for other image
    /// formats it returns the path so Avalonia decodes natively.
    /// </summary>
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not AssetInfo info) return null;

        var ext = System.IO.Path.GetExtension(info.FullPath).ToLowerInvariant();
        if (ext == ".texture")
        {
            if (_cache.TryGetValue(info.FullPath, out var weak) && weak.TryGetTarget(out var cached))
                return cached;

            if (TextureTexrReader.TryDecodeBitmap(info.FullPath, out var bmp, out var decodeError) && bmp != null)
            {
                _cache[info.FullPath] = new WeakReference<Avalonia.Media.Imaging.Bitmap>(bmp);
                return bmp;
            }
            PrimalEditor_Avalonia.Common.Logger.Warning($"Thumbnail decode failed for {info.FullPath}: {decodeError}");
            return null;
        }

        return info.FullPath;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

public class FileTypeToIconConverter : IValueConverter
{
    private static readonly StreamGeometry MeshIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,10 L30,10 L20,30 Z");
    // Model = three stacked meshes (signals "container with submeshes").
    private static readonly StreamGeometry ModelIcon = StreamGeometry.Parse("M0,5 L40,5 L40,15 L0,15 Z M0,18 L40,18 L40,28 L0,28 Z M10,31 L30,31 L20,40 Z");
    private static readonly StreamGeometry TextureIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M5,5 L35,5 L35,35 L5,35 Z M5,5 L20,20 L35,5 M5,35 L20,20 L35,35");
    private static readonly StreamGeometry AudioIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,5 L10,35 M20,10 L20,30 M30,15 L30,25");
    private static readonly StreamGeometry ScriptIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,5 L30,5 M10,15 L30,15 M10,25 L25,25 M10,35 L20,35");
    private static readonly StreamGeometry FileIcon = StreamGeometry.Parse("M0,0 L25,0 L40,15 L40,40 L0,40 Z M25,0 L25,15 L40,15");
    // Folder = standard "manila folder" outline (rectangle with a tab).
    private static readonly StreamGeometry FolderIcon = StreamGeometry.Parse("M0,12 L0,38 L0,40 L2,40 L38,40 L40,40 L40,16 L40,14 L22,14 L18,8 L18,8 L18,6 L4,6 L2,6 L0,8 L0,12 Z");
    // Particle = burst: core ring + radiating sparks (distinct from folders
    // and documents; used for both PFX emitters and legacy presets).
    private static readonly StreamGeometry ParticleBurstIcon = StreamGeometry.Parse(
        "M20,13 A7,7 0 1 0 20,27 A7,7 0 1 0 20,13 Z " +
        "M20,2 L20,8 M20,32 L20,38 M2,20 L8,20 M32,20 L38,20 " +
        "M7,7 L11,11 M29,29 L33,33 M33,7 L29,11 M11,29 L7,33");

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return value switch
        {
            AssetType.Folder => FolderIcon,
            AssetType.Model => ModelIcon,
            AssetType.Mesh => MeshIcon,
            AssetType.Texture => TextureIcon,
            AssetType.Audio => AudioIcon,
            AssetType.Script => ScriptIcon,
            AssetType.PfxEmitter or AssetType.ParticlePreset => ParticleBurstIcon,
            _ => FileIcon,
        };
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

public static class PreviewAssetTypeConverters
{
    public static readonly IValueConverter IsTexture = new AssetTypeEqualityConverter { Target = AssetType.Texture };
    public static readonly IValueConverter IsMesh = new AssetTypeEqualityConverter { Target = AssetType.Mesh };
    public static readonly IValueConverter IsModel = new AssetTypeEqualityConverter { Target = AssetType.Model };
    public static readonly IValueConverter IsMeshOrModel = new AssetTypeInSetConverter {
        Targets = new HashSet<AssetType> { AssetType.Mesh, AssetType.Model }
    };
    public static readonly IValueConverter IsMaterial = new AssetTypeEqualityConverter { Target = AssetType.Material };
    public static readonly IValueConverter IsScript = new AssetTypeEqualityConverter { Target = AssetType.Script };
    public static readonly IValueConverter IsShader = new AssetTypeEqualityConverter { Target = AssetType.Shader };
    public static readonly IValueConverter IsAudio = new AssetTypeEqualityConverter { Target = AssetType.Audio };
}

public class AssetTypeEqualityConverter : IValueConverter
{
    public AssetType Target { get; set; }

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is AssetType t && t == Target;

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

public class AssetTypeInSetConverter : IValueConverter
{
    public HashSet<AssetType> Targets { get; set; } = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is AssetType t && Targets.Contains(t);

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
