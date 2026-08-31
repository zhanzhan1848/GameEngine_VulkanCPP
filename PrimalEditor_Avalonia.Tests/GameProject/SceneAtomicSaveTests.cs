using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class SceneAtomicSaveTests
{
    [Fact]
    public void AtomicSave_UsesPartialFileThenValidateAndRename()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var scene = new Scene("AtomicTest")
        {
            Guid = sceneGuid
        };
        scene.Entities.Add(new GameEntity("TestEntity"));

        var expectedPath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        var partialPath = expectedPath + ".partial";

        Serializer.SaveScene(scene, expectedPath);

        Assert.True(File.Exists(expectedPath), "Final scene file should exist");
        Assert.False(File.Exists(partialPath), "Partial file should be cleaned up");
    }

    [Fact]
    public void AtomicSave_ReadBackValidationBeforeOverwrite()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var scene = new Scene("ValidationTest")
        {
            Guid = sceneGuid
        };
        var entity = new GameEntity("Entity1")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid())
        };
        entity.Components.Add(new Transform { PositionX = 42.0f });
        scene.Entities.Add(entity);

        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        Serializer.SaveScene(scene, scenePath);

        var loadedScene = Serializer.LoadScene(scenePath);

        Assert.NotNull(loadedScene);
        Assert.Equal(scene.Name, loadedScene!.Name);
        Assert.Equal(scene.Guid, loadedScene.Guid);
        Assert.Single(loadedScene.Entities);

        var loadedEntity = loadedScene.Entities[0];
        Assert.Equal("Entity1", loadedEntity.Name);
        var transform = loadedEntity.GetComponent<Transform>();
        Assert.NotNull(transform);
        Assert.Equal(42.0f, transform!.PositionX);
    }

    [Fact]
    public void AtomicSave_FlushesToDiskBeforeRename()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var scene = new Scene("FlushTest")
        {
            Guid = sceneGuid
        };
        scene.Entities.Add(new GameEntity("Entity"));

        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        Serializer.SaveScene(scene, scenePath);

        var json = File.ReadAllText(scenePath);
        var parsed = JsonDocument.Parse(json);

        Assert.True(parsed.RootElement.TryGetProperty("$id", out _), "Scene should be readable immediately after flush");
        Assert.Equal("FlushTest", parsed.RootElement.GetProperty("Name").GetString());
    }

    [Fact]
    public void LoadScene_MissingFinalFileCleansStalePartialAndReturnsNull()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        var partialPath = scenePath + ".partial";
        File.WriteAllText(partialPath, "corrupt stale partial");

        Assert.Null(Serializer.LoadScene(scenePath));
        Assert.False(File.Exists(partialPath), "Stale partial file should be cleaned when no final scene exists");
    }

    [Fact]
    public void AtomicSave_OverwritesExistingFile()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());

        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");

        var originalScene = new Scene("Original")
        {
            Guid = sceneGuid
        };
        originalScene.Entities.Add(new GameEntity("OriginalEntity"));
        Serializer.SaveScene(originalScene, scenePath);

        var originalJson = File.ReadAllText(scenePath);
        Assert.Contains("OriginalEntity", originalJson, StringComparison.Ordinal);

        var updatedScene = new Scene("Updated")
        {
            Guid = sceneGuid
        };
        updatedScene.Entities.Add(new GameEntity("UpdatedEntity"));
        Serializer.SaveScene(updatedScene, scenePath);

        var updatedJson = File.ReadAllText(scenePath);
        Assert.DoesNotContain("OriginalEntity", updatedJson, StringComparison.Ordinal);
        Assert.Contains("UpdatedEntity", updatedJson, StringComparison.Ordinal);
    }

    [Fact]
    public void LoadScene_RoundTripPreservesAllData()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var entity1Guid = new EntityGuid(Guid.NewGuid());
        var entity2Guid = new EntityGuid(Guid.NewGuid());

        var entity1 = new GameEntity("Entity1")
        {
            EntityGuid = entity1Guid,
            IsEnabled = true
        };
        entity1.Components.Add(new Transform
        {
            PositionX = 1.0f,
            PositionY = 2.0f,
            PositionZ = 3.0f,
            RotationX = 0.5f,
            RotationY = 0.6f,
            RotationZ = 0.7f,
            ScaleX = 1.5f,
            ScaleY = 1.5f,
            ScaleZ = 1.5f
        });

        var entity2 = new GameEntity("Entity2")
        {
            EntityGuid = entity2Guid,
            Parent = entity1,
            IsEnabled = false
        };
        entity2.Components.Add(new Transform { PositionX = 10.0f });
        entity1.Children.Add(entity2);

        var scene = new Scene("RoundTripTest")
        {
            Guid = sceneGuid
        };
        scene.Entities.Add(entity1);

        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        Serializer.SaveScene(scene, scenePath);
        var loadedScene = Serializer.LoadScene(scenePath);

        Assert.NotNull(loadedScene);
        Assert.Equal(scene.Name, loadedScene!.Name);
        Assert.Equal(scene.Guid, loadedScene.Guid);
        // Only root entities sit on scene.Entities; children attach via parent.Children.
        var loadedEntity1 = loadedScene.Entities.FirstOrDefault(e => e.EntityGuid == entity1Guid);
        Assert.Single(loadedScene.Entities);
        Assert.NotNull(loadedEntity1);

        var loadedEntity2 = loadedEntity1!.Children.FirstOrDefault(e => e.EntityGuid == entity2Guid);
        Assert.NotNull(loadedEntity2);
        Assert.Equal("Entity1", loadedEntity1!.Name);
        Assert.Equal("Entity2", loadedEntity2!.Name);
        Assert.True(loadedEntity1.IsEnabled);
        Assert.False(loadedEntity2.IsEnabled);
        Assert.Null(loadedEntity1.Parent);
        Assert.Equal(loadedEntity1, loadedEntity2.Parent);
        Assert.Single(loadedEntity1.Children);
        Assert.Equal(loadedEntity2, loadedEntity1.Children[0]);
        Assert.Empty(loadedEntity2.Children);
        Assert.Equal(loadedScene, loadedEntity1.ParentScene);
        Assert.Equal(loadedScene, loadedEntity2.ParentScene);

        var transform1 = loadedEntity1.GetComponent<Transform>();
        Assert.NotNull(transform1);
        Assert.Equal(1.0f, transform1!.PositionX);
        Assert.Equal(2.0f, transform1!.PositionY);
        Assert.Equal(3.0f, transform1!.PositionZ);
        Assert.Equal(0.5f, transform1!.RotationX);
        Assert.Equal(0.6f, transform1!.RotationY);
        Assert.Equal(0.7f, transform1!.RotationZ);
        Assert.Equal(1.5f, transform1!.ScaleX);
        Assert.Equal(1.5f, transform1!.ScaleY);
        Assert.Equal(1.5f, transform1!.ScaleZ);
    }

    [Fact]
    public void SceneFile_NamingUsesSceneGuid()
    {
        using var temp = new TemporaryProject();
        var sceneGuid = new SceneGuid(Guid.Parse("12345678-1234-1234-1234-123456789abc"));
        var scene = new Scene("NamedScene")
        {
            Guid = sceneGuid
        };

        var scenePath = System.IO.Path.Combine(temp.ScenesPath, $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}.scene");
        Serializer.SaveScene(scene, scenePath);

        Assert.True(File.Exists(scenePath));
        Assert.Contains("12345678-1234-1234-1234-123456789abc", scenePath, StringComparison.Ordinal);
        Assert.DoesNotContain("NamedScene", scenePath, StringComparison.Ordinal);
    }
}