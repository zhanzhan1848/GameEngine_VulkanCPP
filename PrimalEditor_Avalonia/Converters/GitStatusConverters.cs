using System;
using System.Globalization;
using Avalonia;
using Avalonia.Data.Converters;
using Avalonia.Media;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Converters;

/// <summary>
/// A8 源码控制: content-browser status dot bindings off AssetInfo.GitStatus.
/// </summary>
public static class GitStatusConverters
{
    /// <summary>GitStatus != None → dot visible.</summary>
    public static readonly IValueConverter IsVisible =
        new FuncValueConverter<GitService.FileStatus, bool>(s => s != GitService.FileStatus.None);

    /// <summary>Modified/Deleted → orange, Added/Untracked → green.</summary>
    public static readonly IValueConverter ToBrush =
        new FuncValueConverter<GitService.FileStatus, IBrush>(s => s switch
        {
            GitService.FileStatus.Modified or GitService.FileStatus.Deleted
                => new SolidColorBrush(Color.FromRgb(255, 170, 0)),
            GitService.FileStatus.Added or GitService.FileStatus.Untracked
                => new SolidColorBrush(Color.FromRgb(120, 220, 120)),
            _ => Brushes.Transparent,
        });
}
