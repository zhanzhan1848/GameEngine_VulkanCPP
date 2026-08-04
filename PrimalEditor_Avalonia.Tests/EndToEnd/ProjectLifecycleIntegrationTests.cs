using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.EndToEnd;

/// <summary>
/// End-to-end integration covering the "open project → load scenes →
/// create entities → save → close → reopen → verify" loop that the
/// editor needs to round-trip without losing data, including the
/// GUID-stable entity / scene / asset references we rebuilt in the
/// earlier rounds. The tests run without a UI harness — they
/// exercise the same ViewModels the Avalonia bindings talk to, so any
/// regression in the data path surfaces here.
/// </summary>
[Collection("AssetRegistry")]
public sealed class ProjectLifecycleIntegrationTests
{
    [Fact]
    public void OpenProject_PopulatesScenesFromDisk()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var projectRoot = temp.Path;

        // Build a project skeleton the same way NewProject does.
        var project = Project.CreateNew("E2E", projectRoot);
        project.ActiveScene = new Scene("Main");
        project.Scenes.Add(project.ActiveScene);

        var hero = new GameEntity("Hero");
        hero.Components.Add(new Transform { PositionX = 1, PositionY = 2, PositionZ = 3 });
        hero.Components.Add(new LightComponent { Intensity = 4 });
        project.ActiveScene.Entities.Add(hero);

        // Persist via the same code the editor's Save menu uses.
        Serializer.SaveScene(project.ActiveScene,
            Serializer.ScenePath(project, project.ActiveScene.Guid));
        Serializer.SaveProject(project, project.FullPath);

        // Now open from a fresh state and confirm entities re-load.
        var reloaded = OpenProject.Open(project.FullPath);
        Assert.NotNull(reloaded);
        Assert.Single(reloaded!.Scenes);
        var reloadedScene = reloaded.ActiveScene
            ?? Assert.Single(reloaded.Scenes, s => s.Guid == project.ActiveScene!.Guid);
        Assert.Equal("Main", reloadedScene.Name);

        var entity = Assert.Single(reloadedScene.Entities);
        Assert.Equal("Hero", entity.Name);
        Assert.Equal(new EntityGuid(hero.EntityGuid.Value), entity.EntityGuid);

        var transform = entity.GetTransform();
        Assert.NotNull(transform);
        Assert.Equal(1f, transform!.PositionX);
        Assert.Equal(2f, transform.PositionY);
        Assert.Equal(3f, transform.PositionZ);

        var light = entity.GetComponent<LightComponent>();
        Assert.NotNull(light);
        Assert.Equal(4f, light!.Intensity);
    }

    [Fact]
    public void OpenProject_GUIDStability_AfterRescanScenarios()
    {
        AssetRegistry.Reset();
        using var temp = new TemporaryProject();
        var projectRoot = temp.Path;
        var contentDir = temp.ContentPath;

        // Pre-create a material file with a hand-rolled .meta so that
        // AssetRegistry's GUID authority (the .meta) is exercised.
        var materialPath = Path.Combine(contentDir, "Wall.material");
        MaterialSerializer.Write(materialPath, new MaterialDocument
        {
            Name = "Wall",
            ShaderGuid = new AssetGuid(Guid.NewGuid()),
        });

        var info1 = AssetRegistry.Register(materialPath);
        Assert.NotNull(info1);
        var guidAfterFirstRegister = info1!.Guid;

        // Re-registering the same path should yield the same GUID
        // (.meta is the authority, AssetRegistry.GetByPath memoises
        // the existing entry).
        var info2 = AssetRegistry.Register(materialPath);
        Assert.Equal(guidAfterFirstRegister, info2!.Guid);

        // Simulate a SQLite wipe (the editor's Rebuild path) by
        // Resetting the registry. The .meta file should still carry
        // the original GUID forward.
        AssetRegistry.Reset();
        AssetRegistry.Initialize(contentDir, projectRoot);
        var infoAfterReset = AssetRegistry.Get(materialPath);
        Assert.NotNull(infoAfterReset);
        Assert.Equal(guidAfterFirstRegister, infoAfterReset!.Guid);
    }

    [Fact]
    public void MaterialOverride_MultiSelect_EditPropagatesToAll()
    {
        var entity1 = new GameEntity("A");
        entity1.Components.Add(new MaterialOverrideComponent
        {
            Slots = { new MaterialOverrideSlot { Slot = "Albedo" } }
        });
        var entity2 = new GameEntity("B");
        entity2.Components.Add(new MaterialOverrideComponent
        {
            Slots = { new MaterialOverrideSlot { Slot = "Albedo" } }
        });

        var msEntity = new MSEntity();
        msEntity.SelectedEntities.Add(entity1);
        msEntity.SelectedEntities.Add(entity2);
        var ms = new MSMaterialOverride(msEntity);

        var albedoGuid = new AssetGuid(Guid.NewGuid());
        // Apply via the helper the inspector uses.
        ms.ApplyToSelection(0, albedoGuid);

        Assert.Equal(albedoGuid.Value, entity1.GetComponent<MaterialOverrideComponent>()!.Slots[0].MaterialGuid.Value);
        Assert.Equal(albedoGuid.Value, entity2.GetComponent<MaterialOverrideComponent>()!.Slots[0].MaterialGuid.Value);
    }
}
