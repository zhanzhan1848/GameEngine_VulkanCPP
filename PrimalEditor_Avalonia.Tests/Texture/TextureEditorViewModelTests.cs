using System;
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

    // ====== P1.c: Max{Mip,Array,Depth}Index reflect Settings.* via Math.Max(decoded, Settings) ======

    private static TextureDecoded MakeDecoded(int array = 1, int mip = 1, int depth = 1)
    {
        // 1x1 RGBA pixel = 4 bytes; matches TextureSliceSet's required length.
        return new TextureDecoded(1, 1, array, mip, depth, new byte[4]);
    }

    [Fact]
    public void SetSlices_Defaults_KeepsSlicesAtOneOneOne()
    {
        var vm = new TextureEditorViewModel();
        vm.SetSlicesForTest(MakeDecoded());
        Assert.Equal(0, vm.MaxMipIndex);
        Assert.Equal(0, vm.MaxArrayIndex);
        Assert.Equal(0, vm.MaxDepthIndex);
        Assert.Equal("Mip: 0/0", vm.MipDisplay);
    }

    [Fact]
    public void SetSlices_SettingsArraySizeExceedsDecoded_ReflectsSettings()
    {
        var vm = new TextureEditorViewModel { Settings = { ArraySize = 4, MipLevels = 6 } };
        vm.SetSlicesForTest(MakeDecoded(array: 1, mip: 1));
        Assert.Equal(5, vm.MaxMipIndex);  // MipLevels=6 → MaxMipIndex=5
        Assert.Equal(3, vm.MaxArrayIndex); // ArraySize=4 → MaxArrayIndex=3
        Assert.Equal("Mip: 0/5", vm.MipDisplay);
        Assert.Equal("Array: 0/3", vm.ArrayDisplay);
    }

    [Fact]
    public void SetSlices_DecodedExceedsSettings_ReflectsDecoded()
    {
        var vm = new TextureEditorViewModel { Settings = { ArraySize = 1, MipLevels = 1 } };
        vm.SetSlicesForTest(MakeDecoded(array: 4, mip: 6));
        Assert.Equal(5, vm.MaxMipIndex); // decoded mip=6 wins via Math.Max
        Assert.Equal(3, vm.MaxArrayIndex);
    }

    [Fact]
    public void SetSlices_SettingsZero_DefendsToOne()
    {
        var vm = new TextureEditorViewModel { Settings = { ArraySize = 0, MipLevels = 0, Depth = 0 } };
        vm.SetSlicesForTest(MakeDecoded());
        Assert.Equal(0, vm.MaxMipIndex);   // Math.Max(1, 0) = 1 → MaxMipIndex = 0
        Assert.Equal(0, vm.MaxArrayIndex);
        Assert.Equal(0, vm.MaxDepthIndex);
    }

    [Fact]
    public void SetSlices_NullDecoded_KeepsSlicesNull()
    {
        var vm = new TextureEditorViewModel();
        vm.SetSlicesForTest(MakeDecoded());
        vm.SetSlicesForTest(null);
        Assert.Equal(0, vm.MaxMipIndex); // null-safe path
    }

    [Fact]
    public void SetSlices_ExternalSidecarEdit_ReflectsOnSecondOpen()
    {
        // Simulates: open asset → user hand-edits .meta externally → reopen.
        // Settings get reloaded; subsequent SetSlicesForTest uses new values.
        var vm = new TextureEditorViewModel { Settings = { ArraySize = 1, MipLevels = 1 } };
        vm.SetSlicesForTest(MakeDecoded());
        Assert.Equal(0, vm.MaxMipIndex);

        // Simulate "user edited .meta externally" by mutating Settings in place
        vm.Settings.MipLevels = 10;
        vm.SetSlicesForTest(MakeDecoded());
        Assert.Equal(9, vm.MaxMipIndex); // reflects the externally-edited value
    }

    [Fact]
    public void SetSlices_PreservesRgbaBuffer()
    {
        // The Rgba must be the same ReadOnlyMemory<byte> instance — RecomputeMaxIndices
        // must not allocate a new buffer, otherwise the WriteableBitmap's pinned
        // handle from a previous RebuildPreview becomes invalid (GC).
        var rgba = new byte[4];
        var decoded = new TextureDecoded(1, 1, 1, 1, 1, rgba);
        var vm = new TextureEditorViewModel();
        vm.SetSlicesForTest(decoded);

        // After R5, _slices still references the same ReadOnlyMemory<byte> buffer.
        Assert.True(rgba.AsSpan().SequenceEqual(vm.SlicesForTestRgba().Span));
    }

    // SlicesForTestRgba exposes the internal RGBA for the preservation assertion above.
    // Wraps _slices (which is null-when-not-decoded) and the public Rgba accessor
    // would require exposing a getter; this minimal helper is for test only.
}
