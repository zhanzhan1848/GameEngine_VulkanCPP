using Avalonia;
using PrimalEditor_Avalonia.Views;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ContentBrowserDragTests
{
    [Theory]
    [InlineData(0, 0, 0, 0, false)]
    [InlineData(10, 10, 14.9, 10, false)]
    [InlineData(10, 10, 10, 14.9, false)]
    [InlineData(10, 10, 15, 10, true)]
    [InlineData(10, 10, 10, 5, true)]
    public void AssetDrag_RequiresPointerMovementPastThreshold(
        double startX,
        double startY,
        double currentX,
        double currentY,
        bool expected)
    {
        Assert.Equal(expected, ContentBrowserView.HasExceededDragThreshold(
            new Point(startX, startY),
            new Point(currentX, currentY)));
    }
}
