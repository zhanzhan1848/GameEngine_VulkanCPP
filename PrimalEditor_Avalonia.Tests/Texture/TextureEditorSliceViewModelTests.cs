using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Texture;

internal sealed class FakeTextureSliceDecoder : ITextureSliceDecoder
{
    private readonly TextureDecoded? _decoded;
    public FakeTextureSliceDecoder(TextureDecoded? decoded) { _decoded = decoded; }
    public TextureDecoded? TryDecode(string assetPath) => _decoded;
}

public sealed class TextureEditorSliceViewModelTests
{
    [Fact]
    public void WithoutDecoder_AllMaxIndicesAreZero()
    {
        var vm = new TextureEditorViewModel();
        Assert.Equal(0, vm.MaxMipIndex);
        Assert.Equal(0, vm.MaxArrayIndex);
        Assert.Equal(0, vm.MaxDepthIndex);
    }

    [Fact]
    public void ArrayClampsToDecodedCount()
    {
        var decoded = new TextureDecoded(2, 2, 3, 2, 1, new byte[2 * 2 * 4]);
        var vm = new TextureEditorViewModel(new FakeTextureSliceDecoder(decoded));
        vm.SetSlicesForTest(decoded);
        vm.MipIndex = 99;
        vm.ArrayIndex = 99;
        vm.DepthIndex = 99;
        Assert.Equal(1, vm.MaxMipIndex);
        Assert.Equal(2, vm.MaxArrayIndex);
        Assert.Equal(0, vm.MaxDepthIndex);
        Assert.Equal(1, vm.MipIndex);
        Assert.Equal(2, vm.ArrayIndex);
        Assert.Equal(0, vm.DepthIndex);
    }

    [Fact]
    public void InfoReflectsDecodedGeometry()
    {
        var decoded = new TextureDecoded(7, 5, 1, 1, 1, new byte[7 * 5 * 4]);
        var vm = new TextureEditorViewModel(new FakeTextureSliceDecoder(decoded));
        vm.SetSlicesForTest(decoded);
        Assert.Equal(7, vm.Info.Width);
        Assert.Equal(5, vm.Info.Height);
    }

    [Fact]
    public void ClearSlices_RestoresZeroMaxIndices()
    {
        var decoded = new TextureDecoded(2, 2, 2, 2, 1, new byte[2 * 2 * 4]);
        var vm = new TextureEditorViewModel(new FakeTextureSliceDecoder(decoded));
        vm.SetSlicesForTest(decoded);
        vm.MipIndex = 1;
        vm.ArrayIndex = 1;

        vm.SetSlicesForTest(null);

        Assert.Equal(0, vm.MaxMipIndex);
        Assert.Equal(0, vm.MaxArrayIndex);
        Assert.Equal(0, vm.MaxDepthIndex);
        Assert.Equal(0, vm.MipIndex);
        Assert.Equal(0, vm.ArrayIndex);
    }
}
