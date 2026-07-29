using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Texture;

public sealed class TextureEditorViewModelTests
{
    [Fact]
    public void IsNormalMap_DefaultsToFalse()
    {
        var vm = new TextureEditorViewModel();
        Assert.False(vm.IsNormalMap);
        Assert.True(vm.IsSRGB);
    }

    [Fact]
    public void SettingIsNormalMap_DoesNotChangeIsSRGB()
    {
        var vm = new TextureEditorViewModel { IsSRGB = false };
        Assert.False(vm.IsSRGB);

        vm.IsNormalMap = true;

        Assert.True(vm.IsNormalMap);
        Assert.False(vm.IsSRGB);
    }

    [Fact]
    public void SettingIsSRGB_DoesNotChangeIsNormalMap()
    {
        var vm = new TextureEditorViewModel { IsNormalMap = true };
        Assert.True(vm.IsNormalMap);

        vm.IsSRGB = false;

        Assert.True(vm.IsNormalMap);
        Assert.False(vm.IsSRGB);
    }

    [Fact]
    public void Undo_RestoresPreviousIsNormalMap()
    {
        var vm = new TextureEditorViewModel { IsNormalMap = true };
        Assert.True(vm.IsNormalMap);
        Assert.True(vm.IsDirty);

        vm.UndoRedo.Undo();

        Assert.False(vm.IsNormalMap);
    }

    [Fact]
    public void Redo_ReappliesIsNormalMap()
    {
        var vm = new TextureEditorViewModel { IsNormalMap = true };
        vm.UndoRedo.Undo();
        Assert.False(vm.IsNormalMap);

        vm.UndoRedo.Redo();

        Assert.True(vm.IsNormalMap);
    }

    [Fact]
    public void Discard_ReloadsBothFlagsFromSettings()
    {
        var vm = new TextureEditorViewModel
        {
            IsNormalMap = true,
            IsSRGB = false,
        };

        vm.Settings.IsNormalMap = false;
        vm.Settings.IsSRGB = true;

        vm.Discard();

        Assert.False(vm.IsNormalMap);
        Assert.True(vm.IsSRGB);
    }
}
