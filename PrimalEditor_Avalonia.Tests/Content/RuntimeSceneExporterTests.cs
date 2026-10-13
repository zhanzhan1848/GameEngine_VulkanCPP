using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// RuntimeSceneExporter v2 契约测试：cooked mesh 路径（不再写 sidecar）、
/// tex 槽位行来自 MaterialGuids[0] 的材质图解析、路径项目相对化与越界回退。
/// </summary>
[Collection("AssetRegistry")]
public sealed class RuntimeSceneExporterTests
{
    private sealed class Fixture : IDisposable
    {
        public string Root { get; }
        public string ProjectDir { get; }

        public Fixture()
        {
            Root = Path.Combine(Path.GetTempPath(), "PrimalRunExport_" + Guid.NewGuid().ToString("N"));
            ProjectDir = Path.Combine(Root, "MyProject");
            Directory.CreateDirectory(Path.Combine(ProjectDir, "Content", "Assets"));
        }

        /// <summary>资产写完后重扫注册表（Initialize 是全量扫描，先于文件存在调用会得到空表）。</summary>
        public void Rescan() =>
            AssetRegistry.Initialize(Path.Combine(ProjectDir, "Content"), ProjectDir);

        public string WriteAsset(string relativePath, string contents)
        {
            var full = Path.Combine(ProjectDir, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(full)!);
            File.WriteAllText(full, contents);
            return full;
        }

        public string WriteAssetBytes(string relativePath)
        {
            var full = Path.Combine(ProjectDir, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(full)!);
            File.WriteAllBytes(full, new byte[] { 1, 2, 3, 4 });
            return full;
        }

        public string ExportScene(Scene scene) =>
            File.ReadAllText(RuntimeSceneExporter.Export(scene, ProjectDir)!);

        public void Dispose()
        {
            AssetRegistry.Reset();
            Directory.Delete(Root, recursive: true);
        }
    }

    private static Scene SceneWithMeshEntity(string name, AssetGuid meshGuid, AssetGuid? materialGuid)
    {
        var scene = new Scene("ExportTest");
        var entity = new GameEntity(name);
        entity.Components.Add(new Transform());
        var mesh = new MeshRendererComponent { MeshGuid = meshGuid };
        if (materialGuid.HasValue) mesh.MaterialGuids.Add(materialGuid.Value);
        entity.Components.Add(mesh);
        scene.Entities.Add(entity);
        return scene;
    }

    private static string MaterialGraphJson(string? albedoPath, string? normalPath)
    {
        var nodes = "{\"type\":\"MaterialOutput\",\"x\":360,\"y\":0}";
        var albedo = albedoPath == null
            ? string.Empty
            : $",{{\"type\":\"ConstantTexture\",\"x\":-120,\"y\":-96,\"params\":{{\"asset_path\":\"{albedoPath}\"}}}}";
        var normal = normalPath == null
            ? string.Empty
            : $",{{\"type\":\"ConstantTexture\",\"x\":-120,\"y\":96,\"params\":{{\"asset_path\":\"{normalPath}\"}}}}";
        var connections = "";
        var node = 1;
        if (albedoPath != null) { connections += $"{{\"from_node\":{node},\"from_pin\":0,\"to_node\":0,\"to_pin\":4}}"; node++; }
        if (normalPath != null)
        {
            if (connections.Length > 0) connections += ",";
            connections += $"{{\"from_node\":{node},\"from_pin\":0,\"to_node\":0,\"to_pin\":5}}";
        }
        return $"{{\"nodes\":[{nodes}{albedo}{normal}],\"connections\":[{connections}]}}";
    }

    [Fact]
    public void Export_WritesV2HeaderAndCookedMeshRelativePath()
    {
        using var fx = new Fixture();
        var sidecar = fx.WriteAsset("Content/Assets/lion.model.json", "{}");
        fx.WriteAssetBytes("Content/Assets/lion.mesh");
        fx.Rescan();
        var guid = new AssetGuid(AssetRegistry.Get(sidecar)!.Guid);

        var package = fx.ExportScene(SceneWithMeshEntity("lion", guid, materialGuid: null));

        Assert.StartsWith("# primal-runtime-scene v2", package);
        Assert.Contains("mesh \"Content/Assets/lion.mesh\"", package);
        Assert.DoesNotContain("model.json", package);
    }

    [Fact]
    public void Export_OmitsMeshLineWhenNoCookedArtifactExists()
    {
        using var fx = new Fixture();
        // 只有 sidecar，没有 sibling .mesh、没有 .Primal/Cooked 产物。
        var sidecar = fx.WriteAsset("Content/Assets/broken.model.json", "{}");
        fx.Rescan();
        var guid = new AssetGuid(AssetRegistry.Get(sidecar)!.Guid);

        var package = fx.ExportScene(SceneWithMeshEntity("broken", guid, materialGuid: null));

        Assert.DoesNotContain("mesh \"", package);
    }

    [Fact]
    public void Export_WritesTextureSlotsFromFirstMaterialGraph()
    {
        using var fx = new Fixture();
        fx.WriteAsset("Content/Assets/lion.model.json", "{}");
        fx.WriteAssetBytes("Content/Assets/lion.mesh");
        fx.WriteAssetBytes("Content/Assets/tex/albedo.texture");
        fx.WriteAssetBytes("Content/Assets/tex/normal.texture");
        var material = fx.WriteAsset(
            "Content/Assets/lion.material.json",
            "{\"SchemaVersion\":2,\"Name\":\"lion\",\"Textures\":{}}");
        // MaterialWorldTextureResolver 的图 sidecar 命名：ChangeExtension 到
        // .material.graph.json → "lion.material.material.graph.json"。
        fx.WriteAsset(
            "Content/Assets/lion.material.material.graph.json",
            MaterialGraphJson("Content/Assets/tex/albedo.texture", "Content/Assets/tex/normal.texture"));
        fx.Rescan();

        var scene = new Scene("TexTest");
        var entity = new GameEntity("lion");
        entity.Components.Add(new Transform());
        var meshGuid = new AssetGuid(AssetRegistry.Get(
            Path.Combine(fx.ProjectDir, "Content/Assets/lion.model.json"))!.Guid);
        entity.Components.Add(new MeshRendererComponent
        {
            MeshGuid = meshGuid,
        });
        ((MeshRendererComponent)entity.Components[^1]).MaterialGuids.Add(
            new AssetGuid(AssetRegistry.Get(material)!.Guid));
        scene.Entities.Add(entity);

        var package = fx.ExportScene(scene);

        Assert.Contains("tex 0 \"Content/Assets/tex/albedo.texture\"", package);
        Assert.Contains("tex 1 \"Content/Assets/tex/normal.texture\"", package);
        Assert.DoesNotContain("tex 2", package); // ORM 空洞省行
    }

    [Fact]
    public void Export_WritesScriptAndParamLines()
    {
        using var fx = new Fixture();
        fx.WriteAsset("Content/Assets/plain.model.json", "{}");
        fx.WriteAssetBytes("Content/Assets/plain.mesh");
        var scriptPath = fx.WriteAsset("Content/Scripts/rotate.lua", "return {}");
        fx.Rescan();
        var meshGuid = new AssetGuid(AssetRegistry.Get(
            Path.Combine(fx.ProjectDir, "Content/Assets/plain.model.json"))!.Guid);
        var scriptGuid = new AssetGuid(AssetRegistry.Get(scriptPath)!.Guid);

        var scene = new Scene("ScriptTest");
        var entity = new GameEntity("spinner");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { MeshGuid = meshGuid });
        var lua = new PrimalEditor_Avalonia.Components.LuaScriptComponent
        {
            ScriptAssetGuid = scriptGuid,
            ScriptName = "rotate.lua",
        };
        lua.Parameters["speed"] = "0.8";
        lua.Parameters["bad key"] = "rejected";
        entity.Components.Add(lua);
        scene.Entities.Add(entity);

        var package = fx.ExportScene(scene);

        Assert.Contains("script \"Content/Scripts/rotate.lua\"", package);
        Assert.Contains("param speed \"0.8\"", package);
        Assert.DoesNotContain("param bad key", package); // 含空格键被拒
    }

    [Fact]
    public void Export_OmitsTexLinesWithoutMaterial()
    {
        using var fx = new Fixture();
        var sidecar = fx.WriteAsset("Content/Assets/plain.model.json", "{}");
        fx.WriteAssetBytes("Content/Assets/plain.mesh");
        fx.Rescan();
        var guid = new AssetGuid(AssetRegistry.Get(sidecar)!.Guid);

        var package = fx.ExportScene(SceneWithMeshEntity("plain", guid, materialGuid: null));

        Assert.DoesNotContain("tex ", package);
    }

    [Fact]
    public void Export_FallsBackToAbsolutePathForAssetsOutsideProjectRoot()
    {
        using var fx = new Fixture();
        // 材质图引用项目根之外的绝对纹理路径 → tex 行回退绝对路径。
        var outside = Path.Combine(Path.GetTempPath(), "PrimalRunOutside_" + Guid.NewGuid().ToString("N") + ".texture");
        File.WriteAllBytes(outside, new byte[] { 1 });
        try
        {
            fx.WriteAsset("Content/Assets/abs.model.json", "{}");
            fx.WriteAssetBytes("Content/Assets/abs.mesh");
            var material = fx.WriteAsset(
                "Content/Assets/abs.material.json",
                "{\"SchemaVersion\":2,\"Name\":\"abs\",\"Textures\":{}}");
            fx.WriteAsset(
                "Content/Assets/abs.material.material.graph.json",
                MaterialGraphJson(outside.Replace("\\", "/"), null));
            fx.Rescan();
            var meshGuid = new AssetGuid(AssetRegistry.Get(
                Path.Combine(fx.ProjectDir, "Content/Assets/abs.model.json"))!.Guid);
            var scene = new Scene("AbsTest");
            var entity = new GameEntity("abs");
            entity.Components.Add(new Transform());
            var mesh = new MeshRendererComponent { MeshGuid = meshGuid };
            mesh.MaterialGuids.Add(new AssetGuid(AssetRegistry.Get(material)!.Guid));
            entity.Components.Add(mesh);
            scene.Entities.Add(entity);

            var package = fx.ExportScene(scene);

            Assert.Contains($"tex 0 \"{outside.Replace("\\", "/")}\"", package);
        }
        finally
        {
            File.Delete(outside);
        }
    }

    // ---- release S1：builtin 行 + portable 可搬移包 ----

    [Fact]
    public void Export_WritesBuiltinLineForBuiltinMeshEntity()
    {
        using var fx = new Fixture();
        fx.Rescan();

        var scene = new Scene("BuiltinTest");
        var entity = new GameEntity("cube");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Box });
        scene.Entities.Add(entity);

        var package = fx.ExportScene(scene);

        Assert.Contains("builtin box", package);
        Assert.DoesNotContain("mesh \"", package); // 无资产行
    }

    [Fact]
    public void Export_Portable_CopiesAssetsAndRewritesPaths()
    {
        using var fx = new Fixture();
        var sidecar = fx.WriteAsset("Content/Assets/plain.model.json", "{}");
        fx.WriteAssetBytes("Content/Assets/plain.mesh");
        fx.WriteAsset("Content/Assets/tex/albedo.texture", new string('a', 16));
        var scriptPath = fx.WriteAsset("Content/Scripts/rotate.lua", "return {}");
        fx.WriteAsset(
            "Content/Assets/tex/plain.material.material.graph.json",
            MaterialGraphJson("Content/Assets/tex/albedo.texture", null));
        var material = fx.WriteAsset(
            "Content/Assets/tex/plain.material.json",
            "{\"SchemaVersion\":2,\"Name\":\"plain\",\"Textures\":{}}");
        fx.Rescan();
        var meshGuid = new AssetGuid(AssetRegistry.Get(sidecar)!.Guid);
        var scriptGuid = new AssetGuid(AssetRegistry.Get(scriptPath)!.Guid);

        var scene = new Scene("PortableTest");
        var entity = new GameEntity("spinner");
        entity.Components.Add(new Transform());
        var mesh = new MeshRendererComponent { MeshGuid = meshGuid };
        mesh.MaterialGuids.Add(new AssetGuid(AssetRegistry.Get(material)!.Guid));
        entity.Components.Add(mesh);
        var lua = new PrimalEditor_Avalonia.Components.LuaScriptComponent
        {
            ScriptAssetGuid = scriptGuid,
            ScriptName = "rotate.lua",
        };
        entity.Components.Add(lua);
        scene.Entities.Add(entity);

        var path = RuntimeSceneExporter.Export(scene, fx.ProjectDir, portable: true)!;
        var package = File.ReadAllText(path);
        var packageDir = Path.GetDirectoryName(path)!;
        var assetsDir = Path.Combine(packageDir, RuntimeSceneExporter.PortableAssetsDirName);

        // 行内路径全部包相对（_assets/…），无项目相对/绝对残留
        Assert.Contains("mesh \"_assets/", package);
        Assert.Contains("tex 0 \"_assets/", package);
        Assert.Contains("script \"_assets/", package);
        Assert.DoesNotContain("Content/Assets", package);
        Assert.DoesNotContain("Content/Scripts", package);

        // 副本真实落地且内容一致；目录自包含（脚本+mesh+texture）
        var copied = Directory.GetFiles(assetsDir).Select(Path.GetFileName).ToArray();
        Assert.Contains("plain.mesh", copied);
        Assert.Contains("rotate.lua", copied);
        Assert.Contains("albedo.texture", copied);
        Assert.Equal(
            new string('a', 16),
            File.ReadAllText(Path.Combine(assetsDir, "albedo.texture")));

        // 同一资产二次导出复用副本（路径稳定，重跑可增量覆盖）
        var path2 = RuntimeSceneExporter.Export(scene, fx.ProjectDir, portable: true)!;
        Assert.Equal(path, path2);
        Assert.Equal(package, File.ReadAllText(path2));
    }

    [Fact]
    public void Export_Portable_NameCollisionGetsDistinctCopies()
    {
        using var fx = new Fixture();
        // 两个不同内容的同名 albedo.texture（不同目录）
        fx.WriteAsset("Content/A/Plain.model.json", "{}");
        fx.WriteAssetBytes("Content/A/Plain.mesh");
        fx.WriteAsset("Content/A/m/albedo.texture", new string('x', 8));
        fx.WriteAsset("Content/B/m/albedo.texture", new string('y', 8));
        fx.WriteAsset("Content/A/t.material.json", "{\"SchemaVersion\":2,\"Name\":\"a\",\"Textures\":{}}");
        fx.WriteAsset("Content/B/t.material.json", "{\"SchemaVersion\":2,\"Name\":\"b\",\"Textures\":{}}");
        fx.WriteAsset("Content/A/t.material.material.graph.json", MaterialGraphJson("Content/A/m/albedo.texture", null));
        fx.WriteAsset("Content/B/t.material.material.graph.json", MaterialGraphJson("Content/B/m/albedo.texture", null));
        fx.Rescan();
        // 共用同一 cooked mesh（A），只有材质/纹理不同——碰撞场景只针对 texture。
        var meshGuid = new AssetGuid(AssetRegistry.Get(Path.Combine(fx.ProjectDir, "Content/A/Plain.model.json"))!.Guid);

        var scene = new Scene("CollisionTest");
        foreach (var dir in new[] { "A", "B" })
        {
            var entity = new GameEntity(dir);
            entity.Components.Add(new Transform());
            var mesh = new MeshRendererComponent { MeshGuid = meshGuid };
            mesh.MaterialGuids.Add(new AssetGuid(AssetRegistry.Get(Path.Combine(fx.ProjectDir, $"Content/{dir}/t.material.json"))!.Guid));
            entity.Components.Add(mesh);
            scene.Entities.Add(entity);
        }

        var path = RuntimeSceneExporter.Export(scene, fx.ProjectDir, portable: true)!;
        var package = File.ReadAllText(path);

        // 两个 tex 0 行指向不同的 _assets 副本（同名不互相覆盖）
        var texLines = package.Split('\n').Where(l => l.StartsWith("tex 0 ")).ToArray();
        Assert.Equal(2, texLines.Length);
        Assert.NotEqual(texLines[0], texLines[1]);
        var assetsDir = Path.Combine(Path.GetDirectoryName(path)!, RuntimeSceneExporter.PortableAssetsDirName);
        Assert.Equal(2, Directory.GetFiles(assetsDir, "albedo*.texture").Length);
    }
}
