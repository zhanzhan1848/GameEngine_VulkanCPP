using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Components;

[Collection("AssetRegistry")]
public sealed class MaterialOverrideSerializationTests
{
    [Fact]
    public void MaterialOverride_RoundTripsAndStaysInV2()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Hero");
        var albedoGuid = new AssetGuid(Guid.NewGuid());
        var normalGuid = new AssetGuid(Guid.NewGuid());
        var overrideComp = new MaterialOverrideComponent();
        overrideComp.Slots.Add(new MaterialOverrideSlot { Slot = "Albedo", MaterialGuid = albedoGuid });
        overrideComp.Slots.Add(new MaterialOverrideSlot { Slot = "Normal", MaterialGuid = normalGuid });
        entity.Components.Add(overrideComp);
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var loaded = Serializer.LoadScene(scenePath)!;

        Assert.NotNull(loaded);
        var loadedOverride = loaded.Entities[0].GetComponent<MaterialOverrideComponent>();
        Assert.NotNull(loadedOverride);
        Assert.Equal(2, loadedOverride!.Slots.Count);
        Assert.Equal("Albedo", loadedOverride.Slots[0].Slot);
        Assert.Equal(overrideComp.Slots[0].MaterialGuid, loadedOverride.Slots[0].MaterialGuid);
        Assert.Equal("Normal", loadedOverride.Slots[1].Slot);
    }

    [Fact]
    public void MaterialOverride_OptimizesSerializedJsonToAvoidAbsoluteContent()
    {
        using var temp = new TemporaryProject();
        var scenePath = Path.Combine(temp.ScenesPath, "main.scene");
        var scene = new Scene("Main");
        var entity = new GameEntity("Hero");
        var guid = new AssetGuid(Guid.NewGuid());
        var overrideComp = new MaterialOverrideComponent();
        overrideComp.Slots.Add(new MaterialOverrideSlot { Slot = "Albedo", MaterialGuid = guid });
        entity.Components.Add(overrideComp);
        scene.Entities.Add(entity);

        Serializer.SaveScene(scene, scenePath);
        var json = File.ReadAllText(scenePath);

        Assert.Contains("\"materialOverride\"", json);
        Assert.Contains("\"Slots\"", json);
        Assert.Contains(guid.Value.ToString("D"), json, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain(temp.Path, json, StringComparison.Ordinal);
    }
}
