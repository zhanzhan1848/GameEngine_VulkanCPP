using Avalonia.Input;
using PrimalEditor_Avalonia.Views.Engine3D;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

public sealed class EngineViewportInputTests
{
    [Theory]
    [InlineData(true, false, false, MouseButton.Left)]
    [InlineData(false, true, false, MouseButton.Middle)]
    [InlineData(false, false, true, MouseButton.Right)]
    [InlineData(false, false, false, MouseButton.None)]
    public void ResolvePressedMouseButton_RecognizesAllCameraButtons(
        bool left, bool middle, bool right, MouseButton expected)
    {
        Assert.Equal(expected, EngineViewport3D.ResolvePressedMouseButton(left, middle, right));
    }

    [Theory]
    [InlineData(MouseButton.Middle, true)]
    [InlineData(MouseButton.Right, true)]
    [InlineData(MouseButton.Left, false)]
    [InlineData(MouseButton.None, false)]
    public void IsTranslationDragButton_UsesMiddleAndRight(MouseButton button, bool expected)
    {
        Assert.Equal(expected, EngineViewport3D.IsTranslationDragButton(button));
    }
}
