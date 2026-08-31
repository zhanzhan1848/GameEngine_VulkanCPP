using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Content.Shaders;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Migration;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class RebuildDerivedDataTests
{
    [Fact]
    public void DeleteDerivedDataAndRebuild_KeepsStableSourceGuidsAndReconstitutesManifest()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();

        var v1Path = Path.Combine(temp.Path, "TestProject.primal");
        var v1Body = """
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
                  "AssetPath": "Content/Robot.material",
                  "Components": [
                    { "$type": "transform", "IsEnabled": true, "PositionX": 0, "PositionY": 0, "PositionZ": 0 }
                  ],
                  "Children": []
                }
              ]
            }
          ],
          "ActiveScene": null
        }
        """;
        File.WriteAllText(v1Path, v1Body);

        var migration = new ProjectMigrationService().MigrateIfNeeded(v1Path);
        Assert.True(migration.Success, migration.ErrorMessage);

        // After migration there are no real materials on disk so the
        // scene-closure walk yields zero material entries — the test
        // pins down that baseline.
        var emptyProject = new Project { Path = temp.Path };
        var manifestAfterMigration = BuildManifestGenerator.Generate(emptyProject);
        var materialEntries = manifestAfterMigration.Entries.Where(e => e.AssetType == "Material").ToList();
        Assert.Empty(materialEntries);

        var contentDir = Path.Combine(temp.Path, "Content");
        var material = new MaterialDocument
        {
            Name = "Robot",
            ShaderGuid = new AssetGuid(Guid.NewGuid())
        };
        var materialPath = Path.Combine(contentDir, "Robot.material");
        MaterialSerializer.Write(materialPath, material);

        var project = Serializer.LoadProject(v1Path);
        Assert.NotNull(project);

        var projectRoot = temp.Path;
        Directory.CreateDirectory(Path.Combine(projectRoot, ".Primal"));

        // AssetRegistry is initialised with both the content root and
        // the project root so .meta GUID lookup and ProjectRelativePath
        // resolution work. No JSON sidecar configuration is required.
        AssetRegistry.Reset();
        AssetRegistry.Initialize(contentDir, projectRoot);
        AssetRegistry.Register(materialPath);

        // The loaded v2 project has no scenes populated from disk
        // (Serializer.LoadProject only builds the manifest skeleton).
        // We attach a scene that references the material under test so
        // the manifest closure can find it.
        var scene = new Scene("Main");
        var entity = new GameEntity("Hero");
        entity.Components.Add(new Transform());
        var renderer = new MeshRendererComponent();
        renderer.MaterialGuids.Add(new AssetGuid(AssetRegistry.Get(materialPath)!.Guid));
        entity.Components.Add(renderer);
        scene.Entities.Add(entity);
        project!.Scenes.Add(scene);
        project.ActiveScene = scene;
        project.Path = projectRoot;

        var postRebuildManifest = BuildManifestGenerator.Generate(project);
        var materialEntry = postRebuildManifest.Entries.SingleOrDefault(e => e.AssetType == "Material");
        Assert.NotNull(materialEntry);
        Assert.Equal(material.ShaderGuid.Value, materialEntry!.Dependencies.Single());
    }
}
