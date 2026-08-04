using System;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

public sealed class LightSerializationTests
{
    [Fact]
    public void Scene_RoundTripsLightParameters()
    {
        using var temp = new TemporaryProject();
        var scene = new Scene("Main");
        var entity = new GameEntity("Sun");
        entity.Components.Add(new LightComponent
        {
            LightType = LightType.Directional,
            ColorR = 1f,
            ColorG = .95f,
            ColorB = .85f,
            Intensity = 4f,
            Range = 25f,
            UmbraAngle = .2f,
            PenumbraAngle = .4f,
            CastShadow = true,
            IsEnabled = false
        });
        scene.Entities.Add(entity);
        var path = System.IO.Path.Combine(temp.ScenesPath, scene.Guid.Value + ".scene");

        Serializer.SaveScene(scene, path);
        var light = Serializer.LoadScene(path)!.Entities.Single().GetComponent<LightComponent>();

        Assert.NotNull(light);
        Assert.Equal(LightType.Directional, light!.LightType);
        Assert.Equal(4f, light.Intensity);
        Assert.True(light.CastShadow);
        Assert.False(light.IsEnabled);
    }

    [Fact]
    public void Light_RejectsInvalidParameters()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new LightComponent { Intensity = -1 });
        Assert.Throws<ArgumentOutOfRangeException>(() => new LightComponent { Range = -1 });
    }
}
