using System;
using System.Globalization;
using Avalonia.Data.Converters;

namespace PrimalEditor_Avalonia.Common.Converters;

/// <summary>
/// Two-way <see cref="float"/> ↔ <see cref="string"/> converter for
/// <see cref="Avalonia.Controls.TextBox.Text"/> bindings. Used by the
/// material editor's per-component param inputs so the user sees plain
/// numeric text (no <c>NumericUpDown</c> arrows) and we still push the
/// parsed value back into the VM on LostFocus.
///
/// <para>
/// <c>ConvertBack</c> returns <see cref="Avalonia.AvaloniaProperty.UnsetValue"/>
/// when the input doesn't parse — Avalonia then leaves the source binding
/// alone instead of clobbering a partially-typed value ("1." during edit).
/// </para>
/// </summary>
public sealed class StringToFloatConverter : IValueConverter
{
    public static readonly StringToFloatConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return value switch
        {
            float f => f.ToString("0.###", culture),
            double d => d.ToString("0.###", culture),
            _ => string.Empty,
        };
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not string s) return Avalonia.AvaloniaProperty.UnsetValue;
        if (string.IsNullOrWhiteSpace(s)) return 0f;
        if (float.TryParse(s, NumberStyles.Float, culture, out var f)) return f;
        return Avalonia.AvaloniaProperty.UnsetValue;
    }
}