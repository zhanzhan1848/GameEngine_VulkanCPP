using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using System.Collections.Generic;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class PropertiesViewModelEnvironmentTests
{
    [Fact]
    public void SceneEnvironment_RaisesConcretePropertyChanges_AfterAlreadyDirty()
    {
        var environment = new SceneEnvironment();
        var changed = new List<string?>();
        environment.PropertyChanged += (_, args) => changed.Add(args.PropertyName);

        environment.SkyboxEnabled = true;
        changed.Clear(); // Environment is now dirty; the next edit must still notify.
        environment.EnvMapPath = "/tmp/studio.texture";

        Assert.Contains(nameof(SceneEnvironment.EnvMapPath), changed);
    }

    [Fact]
    public void EnvMapPath_RetainsDroppedAsset_WhenEnvironmentViewModelIsNotResolved()
    {
        var properties = new PropertiesViewModel();

        properties.EnvMapPath = "/project/Content/studio.texture";

        Assert.Equal("/project/Content/studio.texture", properties.EnvMapPath);
    }

    [Fact]
    public void EnvMapPath_UpdatesInjectedSceneEnvironment_AndEnablesSkybox()
    {
        var environment = new SceneEnvironment();
        var environmentVm = new SceneEnvironmentViewModel();
        environmentVm.Attach(environment);
        var properties = new PropertiesViewModel
        {
            SceneEnvironment = environmentVm,
        };

        properties.EnvMapPath = "/project/Content/studio.texture";

        Assert.Equal("/project/Content/studio.texture", environment.EnvMapPath);
        Assert.True(environment.SkyboxEnabled);
        Assert.Equal("/project/Content/studio.texture", properties.EnvMapPath);
    }
}
