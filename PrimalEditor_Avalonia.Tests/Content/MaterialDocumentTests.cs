using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class MaterialDocumentTests
{
    [Fact]
    public void Material_RoundTripsAuthoritativeFieldsWithShaderAndTextureGuids()
    {
        using var temp = new TemporaryProject();
        var material = new MaterialDocument
        {
            Name = "RobotPaint",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
            RenderState = new MaterialRenderState
            {
                Surface = "Opaque",
                CullMode = "Back",
                DepthWrite = true
            },
            Parameters = new Dictionary<string, MaterialParameterValue>
            {
                ["BaseColor"] = new MaterialColorParameter { R = 1, G = 1, B = 1, A = 1 },
                ["Roughness"] = new MaterialScalarParameter { Scalar = .6f },
                ["Metallic"] = new MaterialScalarParameter { Scalar = .1f }
            },
            Textures = new Dictionary<string, AssetGuid>
            {
                ["Albedo"] = new AssetGuid(Guid.NewGuid()),
                ["Normal"] = new AssetGuid(Guid.NewGuid())
            }
        };
        var path = Path.Combine(temp.Path, "Robot.material");

        MaterialSerializer.Write(path, material);
        var loaded = MaterialSerializer.Read(path);

        Assert.NotNull(loaded);
        Assert.Equal(material.ShaderGuid.Value, loaded!.ShaderGuid.Value);
        Assert.Equal(material.Textures["Albedo"].Value, loaded.Textures["Albedo"].Value);
        Assert.Equal(material.Textures["Normal"].Value, loaded.Textures["Normal"].Value);
        Assert.Equal(3, loaded.Parameters.Count);
        Assert.Equal(0.6f, loaded.GetScalar("Roughness"));
    }

    [Fact]
    public void Material_DependencyExtractionReturnsShaderAndTextureGuids()
    {
        var shader = new AssetGuid(Guid.NewGuid());
        var albedo = new AssetGuid(Guid.NewGuid());
        var normal = new AssetGuid(Guid.NewGuid());
        var material = new MaterialDocument
        {
            ShaderGuid = shader,
            Textures = new Dictionary<string, AssetGuid> { ["Albedo"] = albedo, ["Normal"] = normal }
        };

        var deps = material.EnumerateDependencies().Select(g => g.Value).ToList();

        Assert.Equal(3, deps.Count);
        Assert.Contains(shader.Value, deps);
        Assert.Contains(albedo.Value, deps);
        Assert.Contains(normal.Value, deps);
    }

    [Fact]
    public void Material_WriteAcceptsBlankMaterialWithoutShaderBinding()
    {
        using var temp = new TemporaryProject();
        var path = Path.Combine(temp.Path, "Broken.material");
        var material = new MaterialDocument
        {
            Name = "Broken"
        };
        // Write succeeds for a blank material — the editor's Save button
        // must work before the user picks a shader binding. Validation
        // (shader compilation) happens at Apply time, not Write time.
        MaterialSerializer.Write(path, material);
        Assert.True(File.Exists(path));
    }

    [Fact]
    public void Material_WriteNewCreatesBlankMaterialWithoutShaderBinding()
    {
        using var temp = new TemporaryProject();
        var path = Path.Combine(temp.Path, "Fresh.material.json");

        MaterialSerializer.WriteNew(path, "Fresh");
        // MaterialSerializer.Read gates on IsValid and returns null
        // for invalid documents. The fresh-blank material is IsValid
        // (schema + name only — no ShaderGuid requirement) so Read
        // round-trips it back to the same struct the editor holds.
        var doc = MaterialSerializer.Read(path);

        Assert.NotNull(doc);
        Assert.Equal("Fresh", doc!.Name);
        Assert.Equal(Guid.Empty, doc.ShaderGuid.Value);
        Assert.True(doc.IsValid, "Blank material is IsValid (schema+name only); shader binding is a later editor step");
    }

    [Fact]
    public void Material_WriteNewDefaultsNameToFileStemWhenBlank()
    {
        using var temp = new TemporaryProject();
        // .material is the single-extension variant; Path.GetFileNameWithoutExtension
        // strips the trailing extension only, so the stem is "ShinyMetal".
        var path = Path.Combine(temp.Path, "ShinyMetal.material");

        MaterialSerializer.WriteNew(path, name: "   ");

        var raw = File.ReadAllText(path);
        Assert.Contains("\"ShinyMetal\"", raw);
    }
}
