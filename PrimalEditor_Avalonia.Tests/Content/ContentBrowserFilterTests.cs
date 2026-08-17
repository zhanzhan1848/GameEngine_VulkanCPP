using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public class ContentBrowserFilterTests
{
    [Theory]
    [InlineData("foo.model.json", true)]
    [InlineData("foo.model", true)]
    [InlineData("foo.mesh", true)]
    [InlineData("foo.texture", true)]
    [InlineData("FOO.MODEL.JSON", true)]   // case insensitive
    [InlineData("path/to/foo.model.json", true)]
    [InlineData("foo.fbx", false)]
    [InlineData("foo.obj", false)]
    [InlineData("foo.gltf", false)]
    [InlineData("foo.glb", false)]
    [InlineData("foo.dae", false)]
    [InlineData("foo.blend", false)]
    [InlineData("foo.png", false)]
    [InlineData("foo.txt", false)]
    public void IsEngineFormat(string path, bool expected)
    {
        Assert.Equal(expected, ContentBrowserFilter.IsEngineFormat(path));
    }

    [Fact]
    public void IsEngineFormat_EmptyString_False()
    {
        Assert.False(ContentBrowserFilter.IsEngineFormat(string.Empty));
    }

    [Fact]
    public void Apply_PassesThroughFolders_AndFiltersFiles()
    {
        var folder = new AssetInfo
        {
            FullPath = Path.Combine(Path.GetTempPath(), "Assets"),
            Type = AssetType.Folder,
        };
        var model = new AssetInfo
        {
            FullPath = Path.Combine(Path.GetTempPath(), "Assets", "foo.model.json"),
            Type = AssetType.Model,
        };
        var fbx = new AssetInfo
        {
            FullPath = Path.Combine(Path.GetTempPath(), "Assets", "bar.fbx"),
            Type = AssetType.Mesh,
        };

        var result = ContentBrowserFilter.Apply(new[] { folder, model, fbx }).ToList();

        Assert.Equal(2, result.Count);
        Assert.Contains(folder, result);
        Assert.Contains(model, result);
        Assert.DoesNotContain(fbx, result);
    }
}