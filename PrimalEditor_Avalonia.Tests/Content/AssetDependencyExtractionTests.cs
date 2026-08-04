using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class AssetDependencyExtractionTests
{
    [Fact]
    public void Material_EnumerateDependencies_ExtractsShaderAndTextures()
    {
        using var temp = new TemporaryProject();
        var shader = new AssetGuid(Guid.NewGuid());
        var albedo = new AssetGuid(Guid.NewGuid());
        var normal = new AssetGuid(Guid.NewGuid());
        var doc = new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = shader,
            Textures = new Dictionary<string, AssetGuid>
            {
                ["Albedo"] = albedo,
                ["Normal"] = normal
            }
        };
        var path = Path.Combine(temp.Path, "Robot.material");
        MaterialSerializer.Write(path, doc);

        var deps = AssetDependencyExtractor.ExtractMaterialDependencies(path).ToList();

        Assert.Equal(3, deps.Count);
        Assert.Contains(shader.Value, deps);
        Assert.Contains(albedo.Value, deps);
        Assert.Contains(normal.Value, deps);
    }

    [Fact]
    public void Shader_EnumerateDependencies_ExtractsSourceVariants()
    {
        using var temp = new TemporaryProject();
        var doc = new ShaderDocument
        {
            Name = "Standard",
            LanguageVariants = new Dictionary<string, string>
            {
                ["Metal"] = "shaders/standard.metal",
                ["HLSL"] = "shaders/standard.hlsl"
            },
            EntryPoints = new Dictionary<string, string>
            {
                ["Vertex"] = "VSMain",
                ["Fragment"] = "PSMain"
            }
        };
        var path = Path.Combine(temp.Path, "standard.shader");
        ShaderSerializer.Write(path, doc);

        var deps = AssetDependencyExtractor.ExtractShaderDependencies(path).ToList();

        Assert.Equal(2, deps.Count);
        Assert.Contains("shaders/standard.metal", deps);
        Assert.Contains("shaders/standard.hlsl", deps);
    }

    [Fact]
    public void AssetRegistry_PopulatesDependenciesForKnownAssetTypes()
    {
        using var temp = new TemporaryProject();
        var projectRoot = temp.Path;
        var contentDir = Path.Combine(projectRoot, "Content");
        Directory.CreateDirectory(contentDir);
        var materialPath = Path.Combine(contentDir, "Robot.material");

        var shader = new AssetGuid(Guid.NewGuid());
        var material = new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = shader
        };
        MaterialSerializer.Write(materialPath, material);

        AssetRegistry.Reset();
        // New AssetRegistry does not maintain a JSON sidecar cache;
        // .meta files are the GUID authority and SQLite is the
        // projected index, so initialization no longer requires any
        // path configuration — pass the project root directly.
        AssetRegistry.Initialize(contentDir, projectRoot);

        var info = AssetRegistry.Get(materialPath);
        Assert.NotNull(info);
        var deps = AssetRegistry.GetDependencies(materialPath).ToList();
        Assert.Contains(shader.Value, deps);
        AssetRegistry.Reset();
    }
}
