using System;
using System.IO;
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
    // Cooked .mesh is now hidden: the model container (.model.json) is
    // the user-visible asset; the .mesh binary is an internal cache.
    [InlineData("foo.mesh", AssetType.Unknown)]
    [InlineData("foo.wav", AssetType.Audio)]
    [InlineData("foo.material", AssetType.Material)]
    [InlineData("foo.shader", AssetType.Shader)]
    [InlineData("foo.model", AssetType.Model)]
    // Multi-extension sidecar that ContentTools actually writes.
    [InlineData("foo.model.json", AssetType.Model)]
    [InlineData("path/to/corridor-corner.model.json", AssetType.Model)]
    [InlineData("foo.txt", AssetType.Unknown)]
    public void GetAssetType_recognizes_engine_and_source_formats(string fileName, AssetType expected)
    {
        Assert.Equal(expected, ContentHelper.GetAssetType(fileName));
    }

    [Fact]
    public void GetAssetType_returns_Folder_for_existing_directory()
    {
        // Use the temp directory as a real on-disk folder.
        var tmp = Path.Combine(Path.GetTempPath(), "PrimalFolder_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tmp);
        try
        {
            Assert.Equal(AssetType.Folder, ContentHelper.GetAssetType(tmp));
        }
        finally
        {
            try { Directory.Delete(tmp, recursive: true); } catch { }
        }
    }

    [Fact]
    public void IsAssetFile_returns_true_for_engine_texture_format()
    {
        Assert.True(ContentHelper.IsAssetFile("/tmp/foo.texture"));
    }
}
