using System;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// Prefab 创建/实例化路径的往返测试（规划 §6.2 A4）：
/// 实体 → EntityDocument → InstantiateEntityDocuments。
/// </summary>
public sealed class PrefabRoundTripTests
{
    [Fact]
    public void Instantiate_PreservesComponentsAndHierarchy()
    {
        var parent = new GameEntity("Parent");
        parent.Components.Add(new Transform
        {
            PositionX = 1f, PositionY = 2f, PositionZ = 3f,
        });
        parent.Components.Add(new LightComponent
        {
            LightType = LightType.Point,
            Intensity = 7f,
        });
        var child = new GameEntity("Child");
        child.Components.Add(new CameraComponent { FieldOfViewDegrees = 45f });
        parent.Children.Add(child);
        child.Parent = parent;

        var documents = PrimalEditor_Avalonia.GameProject.Serializer.ToEntityDocuments(new[] { parent });
        Assert.Equal(2, documents.Count);

        var roots = PrimalEditor_Avalonia.GameProject.Serializer.InstantiateEntityDocuments(
            documents, reassignGuids: true);

        var clone = roots.Single();
        Assert.NotEqual(parent.EntityGuid, clone.EntityGuid);
        Assert.Equal("Parent", clone.Name);

        var transform = clone.GetTransform();
        Assert.NotNull(transform);
        Assert.Equal(1f, transform!.PositionX);
        Assert.Equal(3f, transform.PositionZ);

        var light = clone.GetComponent<LightComponent>();
        Assert.NotNull(light);
        Assert.Equal(7f, light!.Intensity);
        Assert.Equal(LightType.Point, light.LightType);

        var cloneChild = clone.Children.Single();
        Assert.Equal("Child", cloneChild.Name);
        Assert.NotEqual(child.EntityGuid, cloneChild.EntityGuid);
        Assert.Equal(45f, cloneChild.GetComponent<CameraComponent>()!.FieldOfViewDegrees);
    }

    [Fact]
    public void Instantiate_WithoutReassign_KeepsDocumentGuids()
    {
        var source = new GameEntity("Solo");
        source.Components.Add(new Transform());
        var documents = PrimalEditor_Avalonia.GameProject.Serializer.ToEntityDocuments(new[] { source });

        var roots = PrimalEditor_Avalonia.GameProject.Serializer.InstantiateEntityDocuments(
            documents, reassignGuids: false);

        Assert.Equal(source.EntityGuid, roots.Single().EntityGuid);
    }
}
