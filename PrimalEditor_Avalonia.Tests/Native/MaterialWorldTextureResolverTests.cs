using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

public sealed class MaterialWorldTextureResolverTests
{
    [Fact]
    public void Resolve_TracesSampledBaseColorBackToConstantTexture()
    {
        var root = Path.Combine(Path.GetTempPath(), "primal-world-material-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var material = Path.Combine(root, "Test.material.json");
            var texture = Path.Combine(root, "Albedo.texture");
            File.WriteAllText(material, "{}");
            File.WriteAllBytes(texture, new byte[] { 1 });
            File.WriteAllText(Path.ChangeExtension(material, ".material.graph.json"),
                JsonSerializer.Serialize(new
                {
                    nodes = new object[]
                    {
                        new { type = "MaterialOutput" },
                        new { type = "ConstantTexture", @params = new { asset_path = texture } },
                        new { type = "SampleTexture" },
                    },
                    connections = new[]
                    {
                        new { from_node = 1, from_pin = 0, to_node = 2, to_pin = 0 },
                        new { from_node = 2, from_pin = 0, to_node = 0, to_pin = 0 },
                    },
                }));

            var paths = MaterialWorldTextureResolver.Resolve(material);

            Assert.Equal(texture, paths[0]);
            Assert.Null(paths[1]);
            Assert.Null(paths[2]);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Resolve_ReadsExplicitNormalAndOrmTexturePins()
    {
        var root = Path.Combine(Path.GetTempPath(), "primal-world-material-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var material = Path.Combine(root, "Test.material.json");
            var normal = Path.Combine(root, "Normal.texture");
            var orm = Path.Combine(root, "ORM.texture");
            File.WriteAllText(material, "{}");
            File.WriteAllBytes(normal, new byte[] { 1 });
            File.WriteAllBytes(orm, new byte[] { 1 });
            File.WriteAllText(Path.ChangeExtension(material, ".material.graph.json"),
                JsonSerializer.Serialize(new
                {
                    nodes = new object[]
                    {
                        new { type = "MaterialOutput" },
                        new { type = "ConstantTexture", @params = new { asset_path = normal } },
                        new { type = "ConstantTexture", @params = new { asset_path = orm } },
                    },
                    connections = new[]
                    {
                        new { from_node = 1, from_pin = 0, to_node = 0, to_pin = 5 },
                        new { from_node = 2, from_pin = 0, to_node = 0, to_pin = 6 },
                    },
                }));

            var paths = MaterialWorldTextureResolver.Resolve(material);

            Assert.Null(paths[0]);
            Assert.Equal(normal, paths[1]);
            Assert.Equal(orm, paths[2]);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Resolve_TracesThroughMathNodesToFirstUpstreamTexture()
    {
        // BaseColor ← Add ← [SampleTexture(lion), SampleTexture(flag)] is the
        // user's two-texture blend graph. The old resolver stopped at the Add
        // node and silently bound no world texture at all (white fallback).
        // The generic walk must pick the first input's texture deterministically.
        var root = Path.Combine(Path.GetTempPath(), "primal-world-material-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var material = Path.Combine(root, "Test.material.json");
            var lion = Path.Combine(root, "Lion_Albedo.texture");
            var flag = Path.Combine(root, "GL_Flag.texture");
            File.WriteAllText(material, "{}");
            File.WriteAllBytes(lion, new byte[] { 1 });
            File.WriteAllBytes(flag, new byte[] { 1 });

            // Node ids: 0=MaterialOutput, 1=UV, 2=ConstantTexture(lion),
            // 3=SampleTexture(lion), 4=Add.
            File.WriteAllText(Path.ChangeExtension(material, ".material.graph.json"),
                JsonSerializer.Serialize(new
                {
                    nodes = new object[]
                    {
                        new { type = "MaterialOutput" },
                        new { type = "UV" },
                        new { type = "ConstantTexture", @params = new { asset_path = lion } },
                        new { type = "SampleTexture" },
                        new { type = "Add" },
                    },
                    connections = new[]
                    {
                        new { from_node = 1, from_pin = 0, to_node = 3, to_pin = 1 },
                        new { from_node = 2, from_pin = 0, to_node = 3, to_pin = 0 },
                        new { from_node = 3, from_pin = 0, to_node = 4, to_pin = 0 },
                        new { from_node = 1, from_pin = 0, to_node = 4, to_pin = 1 },
                        new { from_node = 4, from_pin = 0, to_node = 0, to_pin = 0 },
                    },
                }));

            var paths = MaterialWorldTextureResolver.Resolve(material);

            Assert.Equal(lion, paths[0]);
            Assert.Null(paths[1]);
            Assert.Null(paths[2]);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Resolve_DeepMathChain_StillFindsTextureAndAvoidsCycles()
    {
        // Multiply(Lerp(A, B), SampleTexture(A).rgba) with a back-reference —
        // deep chain plus the visited guard proving cycles cannot hang resolve.
        var root = Path.Combine(Path.GetTempPath(), "primal-world-material-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var material = Path.Combine(root, "Test.material.json");
            var albedo = Path.Combine(root, "Albedo.texture");
            File.WriteAllText(material, "{}");
            File.WriteAllBytes(albedo, new byte[] { 1 });

            // 0=MaterialOutput, 1=Multiply, 2=Lerp, 3=ConstantTexture,
            // 4=SampleTexture; Multiply output also loops into Lerp pin 2
            // (cycle) and everything funnels to BaseColor.
            File.WriteAllText(Path.ChangeExtension(material, ".material.graph.json"),
                JsonSerializer.Serialize(new
                {
                    nodes = new object[]
                    {
                        new { type = "MaterialOutput" },
                        new { type = "Multiply" },
                        new { type = "Lerp" },
                        new { type = "ConstantTexture", @params = new { asset_path = albedo } },
                        new { type = "SampleTexture" },
                    },
                    connections = new[]
                    {
                        new { from_node = 3, from_pin = 0, to_node = 4, to_pin = 0 },
                        new { from_node = 3, from_pin = 0, to_node = 2, to_pin = 1 },
                        new { from_node = 2, from_pin = 0, to_node = 1, to_pin = 0 },
                        new { from_node = 4, from_pin = 0, to_node = 1, to_pin = 1 },
                        new { from_node = 1, from_pin = 0, to_node = 2, to_pin = 2 },
                        new { from_node = 1, from_pin = 0, to_node = 0, to_pin = 0 },
                    },
                }));

            var paths = MaterialWorldTextureResolver.Resolve(material);

            Assert.Equal(albedo, paths[0]);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
