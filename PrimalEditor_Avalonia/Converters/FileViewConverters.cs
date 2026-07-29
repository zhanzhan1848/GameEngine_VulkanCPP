using System;
using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Converters;

public static class FileViewModeConverters
{
    public static readonly IValueConverter ToThumbnails = new ViewModeToBoolConverter { Target = FileViewMode.Thumbnails };
    public static readonly IValueConverter IsList = new ViewModeToBoolConverter { Target = FileViewMode.List, Inverted = true };
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
}

public class FileTypeToIconConverter : IValueConverter
{
    private static readonly StreamGeometry MeshIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,10 L30,10 L20,30 Z");
    private static readonly StreamGeometry TextureIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M5,5 L35,5 L35,35 L5,35 Z M5,5 L20,20 L35,5 M5,35 L20,20 L35,35");
    private static readonly StreamGeometry AudioIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,5 L10,35 M20,10 L20,30 M30,15 L30,25");
    private static readonly StreamGeometry ScriptIcon = StreamGeometry.Parse("M0,0 L40,0 L40,40 L0,40 Z M10,5 L30,5 M10,15 L30,15 M10,25 L25,25 M10,35 L20,35");
    private static readonly StreamGeometry FileIcon = StreamGeometry.Parse("M0,0 L25,0 L40,15 L40,40 L0,40 Z M25,0 L25,15 L40,15");

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return value switch
        {
            AssetType.Mesh => MeshIcon,
            AssetType.Texture => TextureIcon,
            AssetType.Audio => AudioIcon,
            AssetType.Script => ScriptIcon,
            _ => FileIcon,
        };
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
