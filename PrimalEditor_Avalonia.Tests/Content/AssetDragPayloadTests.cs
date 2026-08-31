using System;
using PrimalEditor_Avalonia.Content;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public class AssetDragPayloadTests
{
    [Fact]
    public void RoundTrip_PreservesAllFields()
    {
        var guid = Guid.NewGuid();
        var parent = Guid.NewGuid();
        var original = new AssetDragPayload(
            Guid: guid,
            FullPath: "/assets/foo/bar.fbx",
            IsContainer: true,
            ParentGuid: parent);

        var json = original.Serialize();
        var parsed = AssetDragPayload.Deserialize(json);

        Assert.NotNull(parsed);
        Assert.Equal(guid, parsed!.Guid);
        Assert.Equal("/assets/foo/bar.fbx", parsed.FullPath);
        Assert.True(parsed.IsContainer);
        Assert.Equal(parent, parsed.ParentGuid);
    }

    [Fact]
    public void RoundTrip_NullParentGuid_StaysNull()
    {
        var original = new AssetDragPayload(
            Guid: Guid.NewGuid(),
            FullPath: "/assets/x.model.json",
            IsContainer: false,
            ParentGuid: null);

        var parsed = AssetDragPayload.Deserialize(original.Serialize());

        Assert.NotNull(parsed);
        Assert.Null(parsed!.ParentGuid);
        Assert.False(parsed.IsContainer);
    }

    [Fact]
    public void Deserialize_Garbage_ReturnsNull()
    {
        Assert.Null(AssetDragPayload.Deserialize("not json"));
    }

    [Fact]
    public void Deserialize_EmptyString_ReturnsNull()
    {
        Assert.Null(AssetDragPayload.Deserialize(string.Empty));
    }
}