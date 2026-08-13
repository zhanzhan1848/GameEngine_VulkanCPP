using System.IO;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

[Collection("AssetRegistry")]
public sealed class ScriptableComponentTests
{
    [Fact]
    public void RotateScript_RoundTripsThroughJson()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Spinning");
        entity.Components.Add(new Transform());
        var rotate = new RotateScriptComponent { SpeedX = "10", SpeedY = "20", SpeedZ = "30" };
        rotate.Parameters["phase"] = "0.5";
        entity.Components.Add(rotate);
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        var loadedRotate = loaded.Entities[0].GetComponent<RotateScriptComponent>();
        Assert.NotNull(loadedRotate);
        Assert.Equal("Rotate", loadedRotate!.ScriptTypeId);
        Assert.Equal("10", loadedRotate.SpeedX);
        Assert.Equal("20", loadedRotate.SpeedY);
        Assert.Equal("30", loadedRotate.SpeedZ);
        Assert.Equal("0.5", loadedRotate.Parameters["phase"]);
    }

    [Fact]
    public void PatrolScript_RoundTripsThroughJson()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Guard");
        entity.Components.Add(new Transform());
        var patrol = new PatrolScriptComponent { Path = "0,0,0;5,0,0;5,0,5", Loop = false };
        entity.Components.Add(patrol);
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        var loadedPatrol = loaded.Entities[0].GetComponent<PatrolScriptComponent>();
        Assert.NotNull(loadedPatrol);
        Assert.Equal("Patrol", loadedPatrol!.ScriptTypeId);
        Assert.Equal("0,0,0;5,0,0;5,0,5", loadedPatrol.Path);
        Assert.False(loadedPatrol.Loop);
    }

    [Fact]
    public void ScriptableComponent_AppearsAsDerivedDiscriminator()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Spinner");
        entity.Components.Add(new RotateScriptComponent());
        scene.Entities.Add(entity);
        Serializer.SaveScene(scene, scenePath);

        var json = File.ReadAllText(scenePath);
        Assert.Contains("\"scriptRotate\"", json);
        Assert.Contains("\"ScriptTypeId\"", json);
    }

    [Fact]
    public void ScriptableComponent_GetMultiselectionComponentDoesNotThrow()
    {
        var entity = new GameEntity("X");
        entity.Components.Add(new RotateScriptComponent());
        var msEntity = new MSEntity();
        msEntity.SelectedEntities.Add(entity);
        var ms = entity.GetComponent<RotateScriptComponent>()!.GetMultiselectionComponent(msEntity);
        Assert.NotNull(ms);
    }
}