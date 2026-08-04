using System;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

public sealed class CameraSerializationTests
{
    [Fact]
    public void Scene_RoundTripsCameraParameters()
    {
        using var temp = new TemporaryProject();
        var scene = new Scene("Main");
        var entity = new GameEntity("Camera");
        entity.Components.Add(new CameraComponent
        {
            Projection = CameraProjection.Perspective,
            FieldOfViewDegrees = 70,
            OrthographicSize = 8,
            NearPlane = .1f,
            FarPlane = 500,
            ClearColorR = .1f,
            ClearColorG = .2f,
            ClearColorB = .3f,
            ClearColorA = 1,
            IsEnabled = true
        });
        scene.Entities.Add(entity);
        var path = System.IO.Path.Combine(temp.ScenesPath, scene.Guid.Value + ".scene");

        Serializer.SaveScene(scene, path);
        var camera = Serializer.LoadScene(path)!.Entities.Single().GetComponent<CameraComponent>();

        Assert.NotNull(camera);
        Assert.Equal(70, camera!.FieldOfViewDegrees);
        Assert.Equal(500, camera.FarPlane);
    }

    [Fact]
    public void Camera_RejectsInvalidClipPlanesAndFieldOfView()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new CameraComponent { FieldOfViewDegrees = 180 });
        var camera = new CameraComponent { NearPlane = 2 };
        Assert.Throws<ArgumentOutOfRangeException>(() => camera.FarPlane = 1);
    }
}
