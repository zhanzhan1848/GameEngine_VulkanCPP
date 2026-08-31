using System;
using System.IO;
using System.Text;
using System.Text.Json;
using PrimalEditor_Avalonia.Content;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ModelSidecarTests
{
    [Fact]
    public void Save_Then_Load_Roundtrips_SubmeshEntries()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "hero.model.json");

        var sidecar = new ModelSidecar
        {
            Source = "/src/hero.fbx",
            Mesh = "/Content/hero.mesh",
            Submeshes =
            {
                new ModelSubmeshEntry { Name = "Body", LodId = 0, LodIndex = 0, MeshIndex = 0, MaterialIndex = 0, VertexCount = 4, IndexCount = 6 },
                new ModelSubmeshEntry { Name = "Head", LodId = 0, LodIndex = 0, MeshIndex = 1, MaterialIndex = 1, VertexCount = 8, IndexCount = 12 },
            }
        };
        sidecar.Save(path);

        var loaded = ModelSidecar.Load(path);
        Assert.NotNull(loaded);
        Assert.Equal(2, loaded!.Submeshes.Count);
        Assert.Equal("Body", loaded.Submeshes[0].Name);
        Assert.Equal(4u, loaded.Submeshes[0].VertexCount);
        Assert.Equal("Head", loaded.Submeshes[1].Name);
        Assert.Equal(12u, loaded.Submeshes[1].IndexCount);
    }

    [Fact]
    public void Load_Returns_Null_For_Missing_File()
    {
        Assert.Null(ModelSidecar.Load("/tmp/does-not-exist-" + Guid.NewGuid() + ".model.json"));
    }

    [Fact]
    public void Load_Returns_Null_For_Malformed_Json()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "bad.model.json");
        File.WriteAllText(path, "{not valid json");
        Assert.Null(ModelSidecar.Load(path));
    }

    [Fact]
    public void Save_Produces_CamelCase_Json()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "camel.model.json");
        new ModelSidecar
        {
            Submeshes = { new ModelSubmeshEntry { Name = "X" } }
        }.Save(path);
        var text = File.ReadAllText(path);
        Assert.Contains("\"submeshes\"", text);
        Assert.Contains("\"vertexCount\"", text);
        Assert.Contains("\"materialIndex\"", text);
    }

    [Fact]
    public void Load_Roundtrips_SubmeshName_With_Quote_Char()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "quote.model.json");
        new ModelSidecar
        {
            Submeshes = { new ModelSubmeshEntry { Name = "Bo\"dy" } }
        }.Save(path);

        var loaded = ModelSidecar.Load(path);
        Assert.NotNull(loaded);
        Assert.Equal("Bo\"dy", loaded!.Submeshes[0].Name);
    }
}
