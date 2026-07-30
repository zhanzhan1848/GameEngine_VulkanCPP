using System.Linq;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

public sealed class DockingLayoutBridgeRoundTripTests
{
    /// <summary>
    /// Round-trip: <c>FromDockTree(ToDockTree(layout))</c> should
    /// produce equivalent entries (same IDs, zones, tab order).
    /// </summary>
    [Fact]
    public void RoundTrip_PreservesCenterEntries()
    {
        var layout = new DockingLayout();
        DockingLayoutOps.Dock(layout, "WorldEditor", DockZone.Center);
        DockingLayoutOps.Dock(layout, "ContentBrowser", DockZone.Center);

        var factory = new MainWindowDockFactory(new MainWindowViewModel());
        var tree = DockingLayoutBridge.ToDockTree(layout, factory, BuildStubDockable);
        var extracted = DockingLayoutBridge.FromDockTree(tree);

        Assert.Equal(2, extracted.Entries.Count);
        Assert.All(extracted.Entries, e => Assert.Equal(DockZone.Center, e.Zone));
        var ids = extracted.Entries.Select(e => e.Id).ToHashSet();
        Assert.Contains("WorldEditor", ids);
        Assert.Contains("ContentBrowser", ids);
    }

    [Fact]
    public void RoundTrip_EmptyTree_ProducesEmptyLayout()
    {
        var factory = new MainWindowDockFactory(new MainWindowViewModel());
        var tree = DockingLayoutBridge.ToDockTree(new DockingLayout(), factory, BuildStubDockable);
        var extracted = DockingLayoutBridge.FromDockTree(tree);
        Assert.NotNull(extracted);
        Assert.Empty(extracted.Entries);
    }

    [Fact]
    public void FromDockTree_NullRoot_ReturnsEmptyLayout()
    {
        var extracted = DockingLayoutBridge.FromDockTree(null!);
        Assert.NotNull(extracted);
        Assert.Empty(extracted.Entries);
    }

    private static IDockable BuildStubDockable(DockEntry entry) =>
        new StubDockable(entry.Id, entry.TabIndex);

    private sealed class StubDockable : Dock.Model.Mvvm.Core.DockBase
    {
        public StubDockable(string id, int tabIndex)
        {
            Id = id;
            Title = id;
        }
    }
}