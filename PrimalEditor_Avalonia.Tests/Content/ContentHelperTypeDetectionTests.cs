using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ContentHelperTypeDetectionTests
{
    [Theory]
    [InlineData("Content/Robot.material", true, "material")]
    [InlineData("Content/shaders/standard.shader", true, "shader")]
    [InlineData("Content/Robot.png", true, "texture")]
    [InlineData("Content/Robot.fbx", true, "mesh")]
    public void ContentHelper_RecognizesMaterialAndShaderAssets(string relative, bool isAsset, string type)
    {
        var ext = Path.GetExtension(relative);
        var detected = PrimalEditor_Avalonia.Content.ContentHelper.GetAssetType(relative);
        var assetFile = PrimalEditor_Avalonia.Content.ContentHelper.IsAssetFile(relative);

        Assert.True(isAsset ? assetFile : !assetFile, $"Asset detection mismatch for {ext}");
        Assert.Equal(type, detected.ToString(), ignoreCase: true);
    }

    [Fact]
    public void Material_LoadAllowsMixedExtensionDiscovery()
    {
        using var temp = new PrimalEditor_Avalonia.Tests.TestInfrastructure.TemporaryProject();
        var path = Path.Combine(temp.Path, "Robot.material");
        var doc = new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = new AssetGuid(System.Guid.NewGuid())
        };
        MaterialSerializer.Write(path, doc);

        Assert.True(File.Exists(path));
        Assert.True(PrimalEditor_Avalonia.Content.ContentHelper.IsAssetFile(path));
    }

    [Fact]
    public void Shader_LoadAllowsMixedExtensionDiscovery()
    {
        using var temp = new PrimalEditor_Avalonia.Tests.TestInfrastructure.TemporaryProject();
        var path = Path.Combine(temp.Path, "standard.shader");
        var doc = new ShaderDocument
        {
            Name = "Standard",
            LanguageVariants = new System.Collections.Generic.Dictionary<string, string>
            {
                ["Metal"] = "standard.metal"
            },
            EntryPoints = new System.Collections.Generic.Dictionary<string, string>
            {
                ["Vertex"] = "VSMain",
                ["Fragment"] = "PSMain"
            }
        };
        ShaderSerializer.Write(path, doc);

        Assert.True(File.Exists(path));
        Assert.True(PrimalEditor_Avalonia.Content.ContentHelper.IsAssetFile(path));
    }
}
