using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class BuildManifestGeneratorTests
{
    [Fact]
    public void Generate_CollectsMeshTextureMaterialShaderWithMaterialDependencies()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentDir);
        // Pass the project root so AssetRegistry can compute
        // ProjectRelativePath for the cooked-artifact lookup.
        AssetRegistry.Initialize(contentDir, temp.Path);

        // Realistic authoring order: shader is registered first, then
        // the material captures its GUID via AssetRegistry.Get. This
        // is the only order that lets the manifest's dependency
        // closure resolve shader and texture references back into
        // AssetRegistry entries (otherwise the GUIDs stay virtual).
        var shaderPath = Path.Combine(contentDir, "standard.shader");
        ShaderSerializer.Write(shaderPath, new ShaderDocument
        {
            Name = "Standard",
            LanguageVariants = { ["Metal"] = "standard.metal" },
            EntryPoints = { ["Vertex"] = "VSMain", ["Fragment"] = "PSMain" }
        });
        var registeredShader = AssetRegistry.Register(shaderPath);
        Assert.NotNull(registeredShader);

        var albedoPath = Path.Combine(contentDir, "robot_albedo.png");
        File.WriteAllBytes(albedoPath, new byte[] { 0x01, 0x02, 0x03 });
        var registeredAlbedo = AssetRegistry.Register(albedoPath);
        Assert.NotNull(registeredAlbedo);

        var materialPath = Path.Combine(contentDir, "Robot.material");
        MaterialSerializer.Write(materialPath, new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = new AssetGuid(registeredShader!.Guid),
            Textures = { ["Albedo"] = new AssetGuid(registeredAlbedo!.Guid) }
        });
        var registeredMaterial = AssetRegistry.Register(materialPath);
        Assert.NotNull(registeredMaterial);

        // The new manifest algorithm walks the dependency closure
        // starting from the default scene. To exercise it we attach a
        // MeshRenderer that references the Material, so both the
        // material and its shader dep get pulled in.
        var project = new Project();
        project.Path = temp.Path;
        var scene = new Scene("Main");
        var entity = new GameEntity("Robot");
        entity.Components.Add(new Transform());
        var renderer = new MeshRendererComponent();
        renderer.MaterialGuids.Add(new AssetGuid(registeredMaterial!.Guid));
        entity.Components.Add(renderer);
        scene.Entities.Add(entity);
        project.Scenes.Add(scene);
        project.ActiveScene = scene;

        var manifest = BuildManifestGenerator.Generate(project);
        var materialEntry = manifest.Entries.SingleOrDefault(e => e.AssetType == "Material");
        var shaderEntry = manifest.Entries.SingleOrDefault(e => e.AssetType == "Shader");
        var textureEntry = manifest.Entries.SingleOrDefault(e => e.AssetType == "Texture");

        Assert.NotNull(materialEntry);
        Assert.NotNull(shaderEntry);
        Assert.NotNull(textureEntry);
        // The shader is reachable through the material's dependency
        // list, and the shader + texture entries both flow into the
        // manifest's closure via the AssetRegistry GUIDs stored in the
        // .material file (not the virtual test GUIDs).
        Assert.Contains(registeredShader.Guid, materialEntry!.Dependencies);
        Assert.Contains(registeredAlbedo.Guid, materialEntry.Dependencies);
    }

    // ---- asset-pipeline S1：层级/脚本/PFX 依赖覆盖 ----

    [Fact]
    public void Generate_CollectsMeshFromChildEntityHierarchy()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentDir);
        AssetRegistry.Initialize(contentDir, temp.Path);

        var sidecar = Path.Combine(contentDir, "part.model.json");
        File.WriteAllText(sidecar, "{}");
        File.WriteAllBytes(Path.Combine(contentDir, "part.mesh"), new byte[] { 1 });
        var mesh = AssetRegistry.Register(sidecar);

        var project = new Project { Path = temp.Path };
        var scene = new Scene("Main");
        var parent = new GameEntity("parent");
        parent.Components.Add(new Transform());
        var child = new GameEntity("child");
        child.Components.Add(new Transform());
        child.Components.Add(new MeshRendererComponent
        {
            MeshGuid = new AssetGuid(mesh!.Guid),
        });
        parent.Children.Add(child); // 深一层：顶层扫描曾静默漏掉
        scene.Entities.Add(parent);
        project.Scenes.Add(scene);

        var manifest = BuildManifestGenerator.Generate(project);

        Assert.Contains(manifest.Entries, e => e.AssetGuid == mesh.Guid);
    }

    [Fact]
    public void Generate_CollectsScriptAndPfxAssets()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentDir);
        AssetRegistry.Initialize(contentDir, temp.Path);

        var luaPath = Path.Combine(contentDir, "spin.lua");
        File.WriteAllText(luaPath, "return {}");
        var lua = AssetRegistry.Register(luaPath);
        var pfxPath = Path.Combine(contentDir, "fx.pfx_emitter");
        File.WriteAllText(pfxPath, "{}");
        var pfx = AssetRegistry.Register(pfxPath);

        var project = new Project { Path = temp.Path };
        var scene = new Scene("Main");
        var entity = new GameEntity("runner");
        entity.Components.Add(new Transform());
        entity.Components.Add(new LuaScriptComponent
        {
            ScriptAssetGuid = new AssetGuid(lua!.Guid),
            ScriptName = "spin.lua",
        });
        var particle = new ParticleFXComponent { AssetPath = pfxPath };
        entity.Components.Add(particle);
        scene.Entities.Add(entity);
        project.Scenes.Add(scene);

        var manifest = BuildManifestGenerator.Generate(project);

        Assert.Contains(manifest.Entries, e => e.AssetGuid == lua.Guid);
        Assert.Contains(manifest.Entries, e => e.AssetGuid == pfx.Guid);
    }

    [Fact]
    public void Generate_SkipsUnregisteredPfxPathWithWarning()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentDir);
        AssetRegistry.Initialize(contentDir, temp.Path);

        var project = new Project { Path = temp.Path };
        var scene = new Scene("Main");
        var entity = new GameEntity("broken");
        entity.Components.Add(new Transform());
        entity.Components.Add(new ParticleFXComponent
        {
            AssetPath = "Content/does_not_exist.pfx_emitter",
        });
        scene.Entities.Add(entity);
        project.Scenes.Add(scene);

        var manifest = BuildManifestGenerator.Generate(project);

        Assert.Empty(manifest.Entries); // 坏路径不中断清单生成
    }
}
