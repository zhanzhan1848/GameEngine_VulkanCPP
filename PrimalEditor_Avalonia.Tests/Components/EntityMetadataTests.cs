using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

[Collection("AssetRegistry")]
public sealed class EntityMetadataTests
{
    [Fact]
    public void EntityMetadata_RoundTripsThroughJson()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Player");
        var metadata = new EntityMetadata
        {
            IsPlayer = true,
            Description = "The main hero"
        };
        metadata.Tags.Add("hero");
        metadata.Tags.Add("spawns-at-start");
        entity.Components.Add(metadata);
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.NotNull(loaded);
        var loadedMetadata = loaded.Entities[0].GetComponent<EntityMetadata>();
        Assert.NotNull(loadedMetadata);
        Assert.True(loadedMetadata!.IsPlayer);
        Assert.Equal("The main hero", loadedMetadata.Description);
        Assert.Equal(2, loadedMetadata.Tags.Count);
        Assert.Contains("hero", loadedMetadata.Tags);
        Assert.Contains("spawns-at-start", loadedMetadata.Tags);
    }

    [Fact]
    public void EntityMetadata_AppearsAsMetadataDiscriminator()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Tagged");
        entity.Components.Add(new EntityMetadata { IsPlayer = false });
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var json = File.ReadAllText(scenePath);

        Assert.Contains("\"metadata\"", json);
        Assert.Contains("\"IsPlayer\"", json);
    }

    [Fact]
    public void EntityMetadata_LoadsBackWithTransformAttached()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Combo");
        entity.Components.Add(new Transform { PositionX = 7 });
        entity.Components.Add(new EntityMetadata { IsPlayer = true });
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.NotNull(loaded.Entities[0].GetTransform());
        Assert.NotNull(loaded.Entities[0].GetComponent<EntityMetadata>());
    }
}
