using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class PropertiesViewModelTransformRefreshTests
{
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
