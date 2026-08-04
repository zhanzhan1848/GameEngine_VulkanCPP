using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ShaderDocumentTests
{
    [Fact]
    public void Shader_RoundTripsDescriptorsWithoutPersistingSourceCode()
    {
        using var temp = new TemporaryProject();
        var document = new ShaderDocument
        {
            Name = "StandardPBR",
            LanguageVariants = new Dictionary<string, string>
            {
                ["Metal"] = "standard_pbr.metal",
                ["HLSL"] = "standard_pbr.hlsl"
            },
            EntryPoints = new Dictionary<string, string>
            {
                ["Vertex"] = "VSMain",
                ["Fragment"] = "PSMain"
            },
            Techniques = new List<string> { "Forward", "Shadow" },
            Keywords = new List<string> { "USE_NORMAL_MAP", "ALPHA_TEST" }
        };
        var path = System.IO.Path.Combine(temp.Path, "standard_pbr.shader");

        ShaderSerializer.Write(path, document);
        var loaded = ShaderSerializer.Read(path)!;
        var json = System.IO.File.ReadAllText(path);

        Assert.NotNull(loaded);
        Assert.Equal("standard_pbr.metal", loaded.LanguageVariants["Metal"]);
        Assert.Equal(new[] { "Forward", "Shadow" }, loaded.Techniques);
        Assert.DoesNotContain("source", json, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Shader_DependencyExtractionReturnsOnlySourceVariants()
    {
        var document = new ShaderDocument
        {
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
        var deps = document.EnumerateDependencies().ToList();
        Assert.Equal(2, deps.Count);
        Assert.Contains("shaders/standard.metal", deps);
        Assert.Contains("shaders/standard.hlsl", deps);
    }

    [Fact]
    public void Shader_WriteRejectsMissingLanguageVariants()
    {
        using var temp = new TemporaryProject();
        var path = System.IO.Path.Combine(temp.Path, "broken.shader");
        var document = new ShaderDocument { Name = "Empty" };
        Assert.Throws<InvalidDataException>(() => ShaderSerializer.Write(path, document));
    }
}
