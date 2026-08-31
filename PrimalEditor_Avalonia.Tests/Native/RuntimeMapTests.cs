using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

[Collection("AssetRegistry")]
public sealed class RuntimeMapTests
{
    [Fact]
    public void EntityMap_StoresAndReusesRuntimeIdByGuid()
    {
        var map = new RuntimeEntityMap();
        var guid = new EntityGuid(Guid.NewGuid());

        map.Register(guid, 42);

        Assert.Equal(42u, map.GetEntityId(guid));
        Assert.True(map.Unregister(guid));
        Assert.Null(map.GetEntityId(guid));
    }

    [Fact]
    public void EntityMap_RejectsZeroAndInvalidIds()
    {
        var map = new RuntimeEntityMap();
        var guid = new EntityGuid(Guid.NewGuid());

        map.Register(guid, 0);
        map.Register(guid, NativeEngineBridge.InvalidEntityId);

        Assert.Null(map.GetEntityId(guid));
    }

    [Fact]
    public void AssetMap_StoresAndReusesContentId()
    {
        var map = new RuntimeAssetMap();
        var guid = new PrimalEditor_Avalonia.Content.AssetGuid(Guid.NewGuid());

        map.Register(guid, 0x1000);

        Assert.Equal(0x1000UL, map.GetContentId(guid));
        map.Clear();
        Assert.Null(map.GetContentId(guid));
    }

	[Fact]
	public void AssetMap_AcceptsFirstNativeContentSlotAndRejectsInvalidId()
	{
		var map = new RuntimeAssetMap();
		var firstGuid = new PrimalEditor_Avalonia.Content.AssetGuid(Guid.NewGuid());
		var invalidGuid = new PrimalEditor_Avalonia.Content.AssetGuid(Guid.NewGuid());

		map.Register(firstGuid, 0);
		map.Register(invalidGuid, NativeEngineBridge.InvalidContentId);

		Assert.Equal(0UL, map.GetContentId(firstGuid));
		Assert.Null(map.GetContentId(invalidGuid));
	}

    [Fact]
    public void MaterialMap_RejectsZeroAndInvalidIds()
    {
        var map = new RuntimeMaterialMap();
        var guid = new AssetGuid(Guid.NewGuid());

        map.Register(guid, 0);
        map.Register(guid, NativeEngineBridge.InvalidEntityId);

        Assert.Null(map.GetRuntimeId(guid));
    }
}
