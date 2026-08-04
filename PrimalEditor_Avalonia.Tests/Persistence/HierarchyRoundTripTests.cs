using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class HierarchyRoundTripTests
{
    // Regression for report item #1: child entities must not duplicate in
    // the saved file after editor mutations that place a child on both
    // scene.Entities and parent.Children (the OnAddEntityClick pattern).
    [Fact]
    public void AddChildThenSaveScene_ThenLoadAgain_DoesNotDuplicateEntityGuids()
    {
        using var temp = new TemporaryProject();
        var scene = new Scene("Hier");
        var sceneGuid = scene.Guid;
        var root = new GameEntity("Root") { ParentScene = scene };
        root.Components.Add(new Transform());
        var childA = new GameEntity("ChildA") { ParentScene = scene };
        childA.Components.Add(new Transform());
        var childB = new GameEntity("ChildB") { ParentScene = scene };
        childB.Components.Add(new Transform());
        scene.Entities.Add(root);
        scene.Entities.Add(childA);
        scene.Entities.Add(childB);
        root.Children.Add(childA);
        root.Children.Add(childB);

        var scenePath = System.IO.Path.Combine(
            temp.ScenesPath,
            $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}{Scene.Extension}");

        Serializer.SaveScene(scene, scenePath);
        var loaded1 = Serializer.LoadScene(scenePath);
        Assert.NotNull(loaded1);

        // Round 2: re-save. The repaired FromSceneDocument loads only
        // root entities into scene.Entities, so loaded1's children live on
        // root.Children only; SaveScene → FlattenEntities writes them once.
        Serializer.SaveScene(loaded1!, scenePath);

        using var doc = JsonDocument.Parse(File.ReadAllText(scenePath));
        var entityGuids = doc.RootElement.GetProperty("Entities")
            .EnumerateArray()
            .Select(e => e.GetProperty("EntityGuid").GetGuid().ToString())
            .ToList();
        Assert.Equal(entityGuids.Count, entityGuids.Distinct().Count());

        var loaded2 = Serializer.LoadScene(scenePath);
        Assert.NotNull(loaded2);
        Assert.Single(loaded2!.Entities);
        Assert.Equal(2, loaded2.Entities[0].Children.Count);
    }
}
