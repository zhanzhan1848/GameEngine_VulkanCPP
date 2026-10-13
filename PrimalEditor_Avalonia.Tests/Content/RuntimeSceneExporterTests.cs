using System;
using System.IO;
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
}
