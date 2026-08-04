using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.Editors.Material;
using PrimalEditor_Avalonia.Editors.Shaders;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

/// <summary>
/// Round-trip tests verifying Material/Shader editors can save a
/// modified document and load it back with all settings intact. These
/// tests exercise the same editor classes that drive the Avalonia GUI,
/// so any regression in the ViewModel's setter / Save / Discard logic
/// surfaces here even without a UI test harness.
/// </summary>
[Collection("AssetRegistry")]
public sealed class AssetEditorRoundTripTests
{
    [Fact]
    public void MaterialEditor_RoundTripPreservesNameAndShader()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var projectRoot = temp.Path;
        var contentDir = temp.ContentPath;

        // Pre-register the shader so its GUID becomes the .meta
        // authority. Editor will read the shader asset via Get.
        var shaderPath = Path.Combine(contentDir, "standard.shader");
        ShaderSerializer.Write(shaderPath, new ShaderDocument
        {
            Name = "Standard",
            EntryPoints = new() { ["Vertex"] = "VSMain", ["Fragment"] = "PSMain" },
            LanguageVariants = new() { ["Metal"] = "shaders/standard.metal" },
        });
        AssetRegistry.Register(shaderPath);
        var registeredShader = AssetRegistry.Get(shaderPath)!.Guid;

        // Material asset with a real shader + texture dependencies.
        var texturePath = Path.Combine(contentDir, "albedo.png");
        File.WriteAllBytes(texturePath, new byte[] { 0xFF, 0xAA, 0x55 });
        AssetRegistry.Register(texturePath);
        var registeredTexture = AssetRegistry.Get(texturePath)!.Guid;

        var materialPath = Path.Combine(contentDir, "Robot.material");
        var editor = new MaterialEditorViewModel();
        var info = new AssetInfo
        {
            FullPath = materialPath,
            Type = AssetType.Material,
            Guid = Guid.NewGuid(),
        };
        editor.SetAsset(info);
        editor.ShaderGuid = new AssetGuid(registeredShader);
        editor.Document.Textures["Albedo"] = new AssetGuid(registeredTexture);
        editor.Surface = "Translucent";
        editor.CullMode = "None";
        editor.DepthTest = false;
        editor.DepthWrite = false;
        editor.Save();

        // Now load a fresh editor and confirm the values stuck.
        var reload = new MaterialEditorViewModel();
        reload.SetAsset(info);
        Assert.Equal("Translucent", reload.Surface);
        Assert.Equal("None", reload.CullMode);
        Assert.False(reload.DepthTest);
        Assert.False(reload.DepthWrite);
        Assert.Equal(registeredShader, reload.ShaderGuid.Value);
        Assert.True(reload.Document.Textures.ContainsKey("Albedo"));
        Assert.Equal(registeredTexture, reload.Document.Textures["Albedo"].Value);

        // Discard should restore the loaded values.
        editor.Document.Textures["Normal"] = new AssetGuid(Guid.NewGuid());
        Assert.True(editor.Document.Textures.ContainsKey("Normal"));
        editor.Discard();
        Assert.False(editor.Document.Textures.ContainsKey("Normal"));
    }

    [Fact]
    public void ShaderEditor_RoundTripPreservesVariantsAndEntryPoints()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = temp.ContentPath;
        var shaderPath = Path.Combine(contentDir, "shader.shader");

        var editor = new ShaderEditorViewModel();
        var info = new AssetInfo
        {
            FullPath = shaderPath,
            Type = AssetType.Shader,
            Guid = Guid.NewGuid(),
        };
        editor.SetAsset(info);
        editor.Name = "MyShader";
        editor.MetalSource = "shaders/my.metal";
        editor.HlslSource = "shaders/my.hlsl";
        editor.VertexEntry = "VSMain";
        editor.FragmentEntry = "PSMain";
        editor.TechniquesCsv = "Base,DepthOfField";
        editor.KeywordsCsv = "OPAQUE,WIREFRAME";
        editor.Save();

        var reload = new ShaderEditorViewModel();
        reload.SetAsset(info);
        Assert.Equal("MyShader", reload.Name);
        Assert.Equal("shaders/my.metal", reload.MetalSource);
        Assert.Equal("shaders/my.hlsl", reload.HlslSource);
        Assert.Equal("VSMain", reload.VertexEntry);
        Assert.Equal("PSMain", reload.FragmentEntry);
        Assert.Equal("Base,DepthOfField", reload.TechniquesCsv);
        Assert.Equal("OPAQUE,WIREFRAME", reload.KeywordsCsv);
    }
}
