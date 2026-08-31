using System;
using Avalonia;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// Wraps asset tiles while allowing an expanded model to own the full row.
/// The model tile is translated back to its original column and its children
/// are therefore measured against the complete browser width.
/// </summary>
public sealed class AssetThumbnailPanel : Panel
{
    internal const double SlotWidth = 80;
    internal const double MinimumRowHeight = 60;

    protected override Size MeasureOverride(Size availableSize)
    {
        var width = ResolveWidth(availableSize.Width);
        var x = 0d;
        var y = 0d;
        var rowHeight = 0d;

        foreach (var child in Children)
        {
            var expanded = IsExpandedModel(child, out var asset);
            if (x > 0 && x + SlotWidth > width)
            {
                y += rowHeight;
                x = 0;
                rowHeight = 0;
            }

            if (expanded)
            {
                asset!.ThumbnailColumnOffset = x;
                child.Measure(new Size(width, double.PositiveInfinity));
                y += Math.Max(rowHeight, Math.Max(MinimumRowHeight, child.DesiredSize.Height));
                x = 0;
                rowHeight = 0;
            }
            else
            {
                if (asset != null) asset.ThumbnailColumnOffset = 0;
                child.Measure(new Size(SlotWidth, double.PositiveInfinity));
                rowHeight = Math.Max(rowHeight, Math.Max(MinimumRowHeight, child.DesiredSize.Height));
                x += SlotWidth;
            }
        }

        return new Size(width, y + rowHeight);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var width = ResolveWidth(finalSize.Width);
        var x = 0d;
        var y = 0d;
        var rowHeight = 0d;

        foreach (var child in Children)
        {
            var expanded = IsExpandedModel(child, out var asset);
            if (x > 0 && x + SlotWidth > width)
            {
                y += rowHeight;
                x = 0;
                rowHeight = 0;
            }

            var height = Math.Max(MinimumRowHeight, child.DesiredSize.Height);
            if (expanded)
            {
                asset!.ThumbnailColumnOffset = x;
                // Keep the expanded item behind the ordinary tiles that share
                // its first row; its transparent full-width area must not hide
                // or steal pointer input from those earlier siblings.
                child.SetValue(ZIndexProperty, -1);
                child.Arrange(new Rect(0, y, width, height));
                y += Math.Max(rowHeight, height);
                x = 0;
                rowHeight = 0;
            }
            else
            {
                if (asset != null) asset.ThumbnailColumnOffset = 0;
                child.SetValue(ZIndexProperty, 0);
                child.Arrange(new Rect(x, y, SlotWidth, height));
                rowHeight = Math.Max(rowHeight, height);
                x += SlotWidth;
            }
        }

        return new Size(width, y + rowHeight);
    }

    private static bool IsExpandedModel(Control child, out AssetInfo? asset)
    {
        asset = child.DataContext as AssetInfo;
        return asset is { IsContainer: true, IsExpanded: true };
    }

    private static double ResolveWidth(double width)
    {
        if (double.IsNaN(width) || double.IsInfinity(width) || width <= 0)
            return SlotWidth;
        return Math.Max(SlotWidth, width);
    }
}
