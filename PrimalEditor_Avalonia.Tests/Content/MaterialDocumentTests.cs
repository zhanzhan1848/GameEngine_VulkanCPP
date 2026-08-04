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
    public void Material_WriteRejectsEmptyShaderGuid()
    {
        using var temp = new TemporaryProject();
        var path = Path.Combine(temp.Path, "Broken.material");
        var material = new MaterialDocument
        {
            Name = "Broken"
        };
        Assert.Throws<InvalidDataException>(() => MaterialSerializer.Write(path, material));
    }
}
