using System.IO;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

[Collection("AssetRegistry")]
public sealed class SceneEnvironmentTests
{
    [Fact]
    public void Environment_RoundTripsThroughSceneJson()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        scene.Environment.AmbientR = 0.2f;
        scene.Environment.AmbientG = 0.3f;
        scene.Environment.AmbientB = 0.4f;
        scene.Environment.AmbientIntensity = 1.5f;
        scene.Environment.FogEnabled = true;
        scene.Environment.FogR = 0.5f;
        scene.Environment.FogNear = 20f;
        scene.Environment.FogFar = 500f;
        scene.Environment.FogDensity = 0.02f;
        scene.Environment.SkyboxEnabled = true;
        scene.Environment.ToneMapping = ToneMappingMode.Reinhard;
        scene.Environment.Exposure = 1.25f;
        var mainCameraGuid = System.Guid.NewGuid();
        scene.Environment.MainCameraEntityGuid = mainCameraGuid;

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.NotNull(loaded);
        Assert.Equal(0.2f, loaded.Environment.AmbientR);
        Assert.Equal(0.3f, loaded.Environment.AmbientG);
        Assert.Equal(0.4f, loaded.Environment.AmbientB);
        Assert.Equal(1.5f, loaded.Environment.AmbientIntensity);
        Assert.True(loaded.Environment.FogEnabled);
        Assert.Equal(0.5f, loaded.Environment.FogR);
        Assert.Equal(20f, loaded.Environment.FogNear);
        Assert.Equal(500f, loaded.Environment.FogFar);
        Assert.Equal(0.02f, loaded.Environment.FogDensity);
        Assert.True(loaded.Environment.SkyboxEnabled);
        Assert.Equal(ToneMappingMode.Reinhard, loaded.Environment.ToneMapping);
        Assert.Equal(1.25f, loaded.Environment.Exposure);
        Assert.Equal(mainCameraGuid, loaded.Environment.MainCameraEntityGuid);
    }

    [Fact]
    public void EditorCamera_RoundTripsPerScene()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "camera.scene");
        var scene = new Scene("Camera");
        scene.EditorCamera.TargetX = 4f;
        scene.EditorCamera.TargetY = -2f;
        scene.EditorCamera.TargetZ = 7f;
        scene.EditorCamera.Distance = 23f;
        scene.EditorCamera.Yaw = 0.75f;
        scene.EditorCamera.Pitch = -0.25f;

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.Equal(4f, loaded.EditorCamera.TargetX);
        Assert.Equal(-2f, loaded.EditorCamera.TargetY);
        Assert.Equal(7f, loaded.EditorCamera.TargetZ);
        Assert.Equal(23f, loaded.EditorCamera.Distance);
        Assert.Equal(0.75f, loaded.EditorCamera.Yaw);
        Assert.Equal(-0.25f, loaded.EditorCamera.Pitch);
    }

    [Fact]
    public void Environment_LoadsAsDefaultWhenJsonMissing()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        // No environment edits — defaults stay in place.

        Serializer.SaveScene(scene, scenePath);

        // Synthesize a "legacy" v2 scene that has no Environment field
        // at all, the way scenes written before this change look.
        var legacyJson = System.IO.File.ReadAllText(scenePath)
            .Replace("\"Environment\":", "\"__LegacyStripped__\":");
        File.WriteAllText(scenePath, legacyJson);

        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.NotNull(loaded);
        Assert.Equal(0.05f, loaded.Environment.AmbientR); // default
        Assert.False(loaded.Environment.FogEnabled);      // default
        Assert.Equal(ToneMappingMode.ACES, loaded.Environment.ToneMapping); // default
        Assert.Equal(1.0f, loaded.Environment.Exposure);   // default
    }

    [Fact]
    public void Environment_DocumentIsValidEvenWhenEnvironmentFieldMissing()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        Serializer.SaveScene(scene, scenePath);

        var legacyJson = System.IO.File.ReadAllText(scenePath)
            .Replace("\"Environment\":", "\"__LegacyStripped__\":");
        File.WriteAllText(scenePath, legacyJson);

        var loaded = Serializer.LoadScene(scenePath);
        Assert.NotNull(loaded); // strict SchemaVersion == 2 must still hold
    }

    [Fact]
    public void Environment_SetterRaisesDirty()
    {
        var env = new SceneEnvironment();
        Assert.False(env.IsDirty);
        env.AmbientR = 0.5f;
        Assert.True(env.IsDirty);
        env.ClearDirty();
        Assert.False(env.IsDirty);
    }

    [Fact]
    public void Environment_MarksSceneDirty()
    {
        var scene = new Scene("Main");
        scene.Environment.FogEnabled = true;
        Assert.True(scene.Environment.IsDirty);
    }
}
