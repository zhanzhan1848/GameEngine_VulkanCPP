using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class PropertiesViewModelTransformRefreshTests
{
    [Fact]
    public void Selection_ExposesOnlyComponentsOwnedBySelectedEntity()
    {
        var lightEntity = new GameEntity("Light");
        lightEntity.Components.Add(new Transform());
        lightEntity.Components.Add(new LightComponent());
        var cameraEntity = new GameEntity("Camera");
        cameraEntity.Components.Add(new Transform());
        cameraEntity.Components.Add(new CameraComponent());
        var properties = new PropertiesViewModel();

        properties.Entities = new[] { lightEntity };

        Assert.NotNull(properties.Transform);
        Assert.True(properties.HasLightComponent);
        Assert.False(properties.HasCameraComponent);
        Assert.False(properties.HasMeshRendererComponent);

        properties.Entities = new[] { cameraEntity };

        Assert.NotNull(properties.Transform);
        Assert.False(properties.HasLightComponent);
        Assert.True(properties.HasCameraComponent);
        Assert.False(properties.HasMeshRendererComponent);
    }

    [Fact]
    public void MixedSelection_ExposesOnlyComponentsCommonToEveryEntity()
    {
        var lightEntity = new GameEntity("Light");
        lightEntity.Components.Add(new Transform());
        lightEntity.Components.Add(new LightComponent());
        var plainEntity = new GameEntity("Plain");
        plainEntity.Components.Add(new Transform());
        var properties = new PropertiesViewModel
        {
            Entities = new[] { lightEntity, plainEntity },
        };

        Assert.NotNull(properties.Transform);
        Assert.False(properties.HasLightComponent);
        Assert.Null(properties.Light);
    }

    [Fact]
    public void InspectorEdit_UsesOwnerWritebackCallbackWithoutMainWindowLookup()
    {
        var entity = new GameEntity("Cube");
        entity.Components.Add(new Transform());
        var properties = new PropertiesViewModel { Entities = new[] { entity } };
        GameEntity[]? pushed = null;
        var dirty = false;
        properties.PushTransformsToNativeCallback = entities => pushed = entities.ToArray();
        properties.MarkProjectDirtyCallback = () => dirty = true;
        properties.UndoRedoQuery = () => null;

        properties.PosX = 17f;

        Assert.Equal(17f, entity.GetTransform()!.PositionX);
        Assert.Equal(new[] { entity }, pushed);
        Assert.True(dirty);
    }

    [Fact]
    public void RefreshFromTransform_NotifiesInspectorForwardingProperties()
    {
        var transform = new Transform
        {
            PositionX = 1f,
            RotationY = 0.25f,
            ScaleZ = 2f,
        };
        var entity = new GameEntity("Selected");
        entity.Components.Add(transform);
        var properties = new PropertiesViewModel
        {
            Entities = new[] { entity },
        };
        var changed = new List<string?>();
        properties.PropertyChanged += (_, e) => changed.Add(e.PropertyName);

        // Matches the gizmo path: mutate the component directly, then ask the
        // inspector VM to refresh its multi-selection wrapper and bindings.
        transform.PositionX = 4.5f;
        transform.RotationY = 0.75f;
        transform.ScaleZ = 3f;
        properties.RefreshFromTransform(transform);

        Assert.Equal(4.5f, properties.PosX);
        Assert.Equal(0.75f, properties.RotY);
        Assert.Equal(3f, properties.ScaleZ);
        Assert.Contains(nameof(PropertiesViewModel.PosX), changed);
        Assert.Contains(nameof(PropertiesViewModel.RotY), changed);
        Assert.Contains(nameof(PropertiesViewModel.ScaleZ), changed);
    }
}
