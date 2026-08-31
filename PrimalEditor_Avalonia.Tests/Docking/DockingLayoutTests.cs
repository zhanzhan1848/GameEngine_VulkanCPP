using System.Linq;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

public sealed class DockingLayoutOpsTests
{
    [Fact]
    public void Dock_NewId_AddsEntry()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Dock(l, "hierarchy", DockZone.Left);
        Assert.Single(l.Entries);
        Assert.Equal(DockZone.Left, l.Entries[0].Zone);
        Assert.False(l.Entries[0].IsFloating);
    }

    [Fact]
    public void Dock_MovesExistingEntry()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Dock(l, "hierarchy", DockZone.Left);
        DockingLayoutOps.Dock(l, "hierarchy", DockZone.Right);
        Assert.Equal(DockZone.Right, l.Entries[0].Zone);
    }

    [Fact]
    public void Float_SetsBoundsAndFlag()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Float(l, "assetTex", 100, 200, 800, 600);
        var e = Assert.Single(l.Entries);
        Assert.True(e.IsFloating);
        Assert.Equal(100, e.X);
        Assert.Equal(200, e.Y);
        Assert.Equal(800, e.Width);
        Assert.Equal(600, e.Height);
    }

    [Fact]
    public void CloseFloat_ReturnsToCenter()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Float(l, "assetTex", 0, 0, 800, 600);
        DockingLayoutOps.CloseFloat(l, "assetTex");
        var e = Assert.Single(l.Entries);
        Assert.False(e.IsFloating);
        Assert.Equal(DockZone.Center, e.Zone);
    }

    [Fact]
    public void PanelsInZone_ExcludesFloating()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Dock(l, "a", DockZone.Left);
        DockingLayoutOps.Dock(l, "b", DockZone.Left);
        DockingLayoutOps.Float(l, "c", 0, 0, 100, 100);
        var ids = DockingLayoutOps.PanelsInZone(l, DockZone.Left).Select(e => e.Id).ToList();
        Assert.Equal(new[] { "a", "b" }, ids);
        Assert.Equal(2, DockingLayoutOps.CountInZone(l, DockZone.Left));
    }

    [Fact]
    public void CompactTabs_ReassignsTabIndicesInInsertionOrder()
    {
        var l = new DockingLayout();
        DockingLayoutOps.Dock(l, "a", DockZone.Right);
        DockingLayoutOps.Dock(l, "b", DockZone.Right);
        DockingLayoutOps.Dock(l, "c", DockZone.Right);
        DockingLayoutOps.Dock(l, "b", DockZone.Right);
        var indices = DockingLayoutOps.PanelsInZone(l, DockZone.Right).Select(e => e.TabIndex).ToList();
        Assert.Equal(new[] { 0, 1, 2 }, indices);
    }
}

public sealed class DockingLayoutSerializerTests
{
    [Fact]
    public void RoundTrip_PreservesEntries()
    {
        var l = new DockingLayout
        {
            ActiveDockableId = "a",
            Entries =
            {
                new DockEntry { Id = "a", Zone = DockZone.Left, TabIndex = 0 },
                new DockEntry { Id = "b", Zone = DockZone.Right, TabIndex = 0, IsFloating = true, X = 10, Y = 20, Width = 800, Height = 600 },
            }
        };
        var json = DockingLayoutSerializer.Save(l);
        var loaded = DockingLayoutSerializer.Load(json);
        Assert.NotNull(loaded);
        Assert.Equal(2, loaded!.Entries.Count);
        Assert.Equal("a", loaded.ActiveDockableId);
        var floating = loaded.Entries.First(e => e.IsFloating);
        Assert.Equal(10, floating.X);
        Assert.Equal(800, floating.Width);
    }

    [Fact]
    public void Load_Invalid_ReturnsNull()
    {
        Assert.Null(DockingLayoutSerializer.Load("{not json"));
        Assert.Null(DockingLayoutSerializer.Load(""));
    }
}
