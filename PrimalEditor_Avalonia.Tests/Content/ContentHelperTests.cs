using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ContentHelperTests
{
    [Theory]
    [InlineData("foo.png", AssetType.Texture)]
    [InlineData("foo.jpg", AssetType.Texture)]
    [InlineData("foo.ktx2", AssetType.Texture)]
    [InlineData("foo.texture", AssetType.Texture)]
    [InlineData("foo.fbx", AssetType.Mesh)]
    [InlineData("foo.obj", AssetType.Mesh)]
    [InlineData("foo.mesh", AssetType.Mesh)]
    [InlineData("foo.wav", AssetType.Audio)]
    [InlineData("foo.material", AssetType.Material)]
    [InlineData("foo.shader", AssetType.Shader)]
    [InlineData("foo.txt", AssetType.Unknown)]
    public void GetAssetType_recognizes_engine_and_source_formats(string fileName, AssetType expected)
    {
        Assert.Equal(expected, ContentHelper.GetAssetType(fileName));
    }

    [Fact]
    public void IsAssetFile_returns_true_for_engine_texture_format()
    {
        Assert.True(ContentHelper.IsAssetFile("/tmp/foo.texture"));
    }
}
