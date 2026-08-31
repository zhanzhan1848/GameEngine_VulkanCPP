using System;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

public sealed class MeshRendererSerializationTests
{
    [Fact]
    public void Scene_RoundTripsMeshAndMaterialGuidsWithoutRuntimeData()
    {
        using var temp = new TemporaryProject();
        var mesh = new AssetGuid(Guid.NewGuid());
        var materialA = new AssetGuid(Guid.NewGuid());
        var materialB = new AssetGuid(Guid.NewGuid());
        var scene = new Scene("Main");
        var entity = new GameEntity("Robot");
        entity.Components.Add(new MeshRendererComponent
        {
            MeshGuid = mesh,
            MaterialGuids = { materialA, materialB },
            CastShadow = false,
            ReceiveShadow = true
        });
        scene.Entities.Add(entity);
        var path = System.IO.Path.Combine(temp.ScenesPath, scene.Guid.Value + ".scene");

        Serializer.SaveScene(scene, path);
        var loaded = Serializer.LoadScene(path)!;
        var renderer = loaded.Entities.Single().GetComponent<MeshRendererComponent>();
        var json = System.IO.File.ReadAllText(path);

        Assert.NotNull(renderer);
        Assert.Equal(mesh, renderer!.MeshGuid);
        Assert.Equal(new[] { materialA, materialB }, renderer.MaterialGuids);
        Assert.False(renderer.CastShadow);
        Assert.True(renderer.ReceiveShadow);
        Assert.DoesNotContain("AssetPath", json, StringComparison.Ordinal);
        Assert.DoesNotContain("ContentId", json, StringComparison.Ordinal);
    }
}
