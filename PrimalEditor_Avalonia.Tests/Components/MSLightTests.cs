using PrimalEditor_Avalonia.Components;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

public sealed class MSLightTests
{
    [Fact]
    public void LightTypeSelection_WritesEnumBackToSelectedLight()
    {
        var light = new LightComponent { LightType = LightType.Point };
        var entity = new GameEntity("Light");
        entity.Components.Add(light);
        var selection = new MSEntity();
        selection.SelectedEntities.Add(entity);
        var multiSelectionLight = new MSLight(selection);

        multiSelectionLight.LightType = LightType.Spot;

        Assert.Equal(LightType.Spot, light.LightType);
    }
}
