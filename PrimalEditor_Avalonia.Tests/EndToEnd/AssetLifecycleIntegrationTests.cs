using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.Content.Prefabs;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.GameProject.Migration;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.EndToEnd;

/// <summary>
/// End-to-end coverage for the asset lifecycle work added after the
/// initial four-rounds-of-fixes: GUID stability under rename, the
/// legacy-meta migration path, diagnostics, the v1→v2 AssetPath →
/// MeshRendererComponent translation, Prefab round-trip, and
/// MaterialOverride parameter overrides.
/// </summary>
[Collection("AssetRegistry")]
public sealed class AssetLifecycleIntegrationTests
{
    [Fact]
    public void RenameAsset_PreservesGUIDAndUpdatesCache()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = temp.ContentPath;
        AssetRegistry.Initialize(contentDir, temp.Path);

        var original = Path.Combine(contentDir, "robot.material");
        MaterialSerializer.Write(original, new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
        });
        var firstInfo = AssetRegistry.Register(original);
        Assert.NotNull(firstInfo);
        var originalGuid = firstInfo!.Guid;
        Assert.NotEqual(Guid.Empty, originalGuid);

        var renamed = AssetRegistry.RenameAsset(original, Path.Combine(contentDir, "mecha.material"));
        Assert.NotNull(renamed);
        Assert.Equal(originalGuid, renamed!.Guid);
        Assert.False(File.Exists(original + ".meta"));
        Assert.True(File.Exists(renamed.FullPath + ".meta"));
        // The asset is reachable by the new path and the old path
        // entry is gone.
        Assert.Null(AssetRegistry.Get(original));
        Assert.NotNull(AssetRegistry.Get(renamed.FullPath));
    }

    [Fact]
    public void LegacyBinaryMetaSidecar_TransparentlyMigratesToJson()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = temp.ContentPath;
        AssetRegistry.Initialize(contentDir, temp.Path);

        var assetPath = Path.Combine(contentDir, "robot_albedo.png");
        File.WriteAllBytes(assetPath, new byte[] { 0x01, 0x02, 0x03 });

        // Write a "PRIM"-prefixed legacy sidecar so the migrator
        // recognises it on the next scan. AssetImportSettingsStore
        // uses 0x4D495250 ('P','R','I','M' little-endian).
        var legacyMeta = assetPath + ".meta";
        using (var fs = File.Create(legacyMeta))
        using (var bw = new BinaryWriter(fs))
        {
            bw.Write(0x4D495250u);
            bw.Write(1u);
            bw.Write(string.Empty);
            bw.Write(1);
            bw.Write(1);
            bw.Write(0.5f);
            bw.Write(false);
            bw.Write(0);
            bw.Write(true);
            bw.Write(true);
        }
        var legacyBytes = File.ReadAllBytes(legacyMeta);
        Assert.True(legacyBytes.Length > 8);

        var info = AssetRegistry.Register(assetPath);
        Assert.NotNull(info);
        Assert.NotEqual(Guid.Empty, info!.Guid);

        // After Register the sidecar should have been rewritten to
        // JSON. Reading the file's first byte must NOT be 'P' (0x50).
        var newBytes = File.ReadAllBytes(legacyMeta);
        Assert.NotEqual(0x50, newBytes[0]);
    }

    [Fact]
    public void Diagnose_DetectsMissingAndDuplicateGuids()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = temp.ContentPath;
        AssetRegistry.Initialize(contentDir, temp.Path);

        var duplicateA = Path.Combine(contentDir, "dup_a.material");
        var duplicateB = Path.Combine(contentDir, "dup_b.material");
        MaterialSerializer.Write(duplicateA, new MaterialDocument
        {
            Name = "A",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
        });
        MaterialSerializer.Write(duplicateB, new MaterialDocument
        {
            Name = "B",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
        });

        var first = AssetRegistry.Register(duplicateA);
        Assert.NotNull(first);
        // Force the same GUID onto B's .meta so the duplicate
        // detector finds a collision.
        var guid = first!.Guid;
        var fakeMetaDoc = new AssetMetaDocument
        {
            Guid = new AssetGuid(guid),
            Importer = "MaterialImporter",
        };
        new JsonAssetMetaStore().Write(duplicateB, fakeMetaDoc);
        AssetRegistry.Register(duplicateB);

        var report = AssetRegistryDiagnosticsRunner.Diagnose();
        Assert.Contains(guid, report.DuplicateAssetGuids);
    }

    [Fact]
    public void Diagnose_ListsUnregisteredFilesInContentRoot()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var contentDir = temp.ContentPath;
        AssetRegistry.Initialize(contentDir, temp.Path);

        var stray = Path.Combine(contentDir, "stray.png");
        File.WriteAllBytes(stray, new byte[] { 0x42 });

        var report = AssetRegistryDiagnosticsRunner.Diagnose();
        Assert.Contains(stray, report.UnregisteredFiles);
    }

    [Fact]
    public void V1Migration_TranslatesAssetPathIntoMeshRenderer()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var projectRoot = temp.Path;
        var contentDir = Path.Combine(projectRoot, "Content");
        Directory.CreateDirectory(contentDir);
        var materialPath = Path.Combine(contentDir, "Models", "robot.material");
        Directory.CreateDirectory(Path.GetDirectoryName(materialPath)!);
        MaterialSerializer.Write(materialPath, new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
        });

        var primal = Path.Combine(projectRoot, "TestProject.primal");
        var v1 = """
        {
          "SchemaVersion": 1,
          "Name": "TestProject",
          "Path": "",
          "BuildConfig": 0,
          "Scenes": [
            {
              "Name": "Main",
              "IsActive": true,
              "Entities": [
                {
                  "Name": "Hero",
                  "IsEnabled": true,
                  "IsActive": true,
                  "AssetPath": "Content/Models/robot.material",
                  "Components": [
                    { "$type": "transform", "IsEnabled": true,
                      "PositionX": 0, "PositionY": 0, "PositionZ": 0 }
                  ],
                  "Children": []
                }
              ]
            }
          ],
          "ActiveScene": null
        }
        """;
        File.WriteAllText(primal, v1);

        var result = new ProjectMigrationService().MigrateIfNeeded(primal);
        Assert.True(result.Success, result.ErrorMessage);

        var reloaded = OpenProject.Open(primal);
        Assert.NotNull(reloaded);
        var scene = reloaded!.ActiveScene;
        Assert.NotNull(scene);
        var entity = Assert.Single(scene!.Entities);
        Assert.Equal("Hero", entity.Name);

        // The v1 AssetPath should now be a real MeshRenderer with a
        // real GUID.
        var meshRenderer = entity.GetComponent<MeshRendererComponent>();
        Assert.NotNull(meshRenderer);
        var info = AssetRegistry.GetByGuid(meshRenderer!.MeshGuid.Value);
        Assert.NotNull(info);
        Assert.Equal(materialPath, info!.FullPath);
    }

    [Fact]
    public void PrefabSerializer_RoundTripsPrefabDocument()
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"prefab_{Guid.NewGuid():N}.prefab");
        try
        {
            var doc = new PrefabDocument
            {
                Name = "RobotPrefab",
                Guid = new AssetGuid(Guid.NewGuid()),
            };
            doc.Entities.Add(new EntityDocument
            {
                EntityGuid = new EntityGuid(Guid.NewGuid()),
                Name = "Hero",
            });
            PrefabSerializer.Write(tmp, doc);
            var reloaded = PrefabSerializer.Read(tmp);
            Assert.NotNull(reloaded);
            Assert.Equal(doc.Guid.Value, reloaded!.Guid.Value);
            Assert.Equal("RobotPrefab", reloaded.Name);
            Assert.Single(reloaded.Entities);
            Assert.Equal("Hero", reloaded.Entities[0].Name);
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
        }
    }

    [Fact]
    public void MaterialOverrideSlot_RoundTripsSubmeshAndParameters()
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"ovr_{Guid.NewGuid():N}.scene");
        try
        {
            var scene = new Scene("Main");
            var entity = new GameEntity("A");
            entity.Components.Add(new Transform());
            var slot = new MaterialOverrideSlot
            {
                Slot = "Albedo",
                SubMeshIndex = 2,
                MaterialGuid = new AssetGuid(Guid.NewGuid()),
            };
            slot.Parameters["Roughness"] = new MaterialParameterOverride
            {
                Type = MaterialParameterType.Scalar,
                Scalar = 0.7f,
            };
            slot.Parameters["Tint"] = new MaterialParameterOverride
            {
                Type = MaterialParameterType.Color,
                R = 1f, G = 0.5f, B = 0.25f, A = 1f,
            };
            entity.Components.Add(new MaterialOverrideComponent { Slots = { slot } });
            scene.Entities.Add(entity);

            Serializer.SaveScene(scene, tmp);
            var loaded = Serializer.LoadScene(tmp)!;
            var loadedOverride = loaded.Entities[0].GetComponent<MaterialOverrideComponent>();
            Assert.NotNull(loadedOverride);
            var loadedSlot = loadedOverride!.Slots[0];
            Assert.Equal(2, loadedSlot.SubMeshIndex);
            Assert.Equal(slot.MaterialGuid.Value, loadedSlot.MaterialGuid.Value);
            Assert.Equal(2, loadedSlot.Parameters.Count);
            Assert.Equal(0.7f, loadedSlot.Parameters["Roughness"].Scalar);
            Assert.Equal(0.25f, loadedSlot.Parameters["Tint"].B);
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
        }
    }

    [Fact]
    public void CrossSceneReferenceComponent_SerializesGuidAndLabel()
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"csr_{Guid.NewGuid():N}.scene");
        try
        {
            var scene = new Scene("Main");
            var entity = new GameEntity("Referrer");
            entity.Components.Add(new Transform());
            var targetGuid = new EntityGuid(Guid.NewGuid());
            entity.Components.Add(new CrossSceneReferenceComponent
            {
                Target = targetGuid,
                Label = "BossDoor",
            });
            scene.Entities.Add(entity);

            Serializer.SaveScene(scene, tmp);
            var loaded = Serializer.LoadScene(tmp)!;
            var csr = loaded.Entities[0].GetComponent<CrossSceneReferenceComponent>();
            Assert.NotNull(csr);
            Assert.Equal(targetGuid, csr!.Target);
            Assert.Equal("BossDoor", csr.Label);
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
        }
    }
}
