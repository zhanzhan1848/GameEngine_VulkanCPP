using System.Collections.Generic;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

public sealed class DockingLayoutBridgeTests
{
    /// <summary>
    /// Minimal test factory: creates Dockable instances keyed by id
    /// string. The bridge only needs the ID-to-Dockable mapping; it
    /// does not care what Dockable is constructed.
    /// </summary>
    private sealed class StubFactory : Factory
    {
        public override IRootDock CreateLayout() => new RootDock { Id = "Stub" };
    }

    private sealed class StubDockable : Dock.Model.Mvvm.Core.DockBase
    {
        public StubDockable(string id)
        {
            Id = id;
            Title = id;
        }
    }

    private static System.Func<DockEntry, IDockable?> IdResolver()
    {
        var map = new Dictionary<string, IDockable>();
        return entry =>
        {
            if (!map.ContainsKey(entry.Id))
                map[entry.Id] = new StubDockable(entry.Id);
            return map[entry.Id];
        };
    }

    [Fact]
    public void ToDockTree_EmptyLayout_ProducesRootWithDocumentsOnly()
    {
        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(new DockingLayout(), factory, IdResolver());

        Assert.Single(root.VisibleDockables);
        var documents = Assert.IsType<DocumentDock>(root.VisibleDockables[0]);
        Assert.Equal("Documents", documents.Id);
        Assert.Empty(documents.VisibleDockables);
    }

    [Fact]
    public void ToDockTree_NullLayout_ProducesRootWithDocumentsOnly()
    {
        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(null!, factory, IdResolver());

        Assert.Single(root.VisibleDockables);
        Assert.IsType<DocumentDock>(root.VisibleDockables[0]);
    }

    [Fact]
    public void ToDockTree_CenterEntries_GoToDocumentDockOrderedByTabIndex()
    {
        var layout = new DockingLayout();
        DockingLayoutOps.Dock(layout, "world", DockZone.Center);
        DockingLayoutOps.Dock(layout, "content", DockZone.Center);
        // Force tab ordering: "content" first, then "world".
        layout.Entries[0].TabIndex = 1;
        layout.Entries[1].TabIndex = 0;

        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(layout, factory, IdResolver());

        var documents = (DocumentDock)root.VisibleDockables[0];
        Assert.Equal(2, documents.VisibleDockables.Count);
        Assert.Equal("content", documents.VisibleDockables[0].Id);
        Assert.Equal("world", documents.VisibleDockables[1].Id);
    }

    [Fact]
    public void ToDockTree_LeftEntries_CreateToolDock()
    {
        var layout = new DockingLayout();
        DockingLayoutOps.Dock(layout, "hierarchy", DockZone.Left);
        DockingLayoutOps.Dock(layout, "inspector", DockZone.Left);

        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(layout, factory, IdResolver());

        Assert.Equal(2, root.VisibleDockables.Count);  // Documents + LeftTools
        var tools = Assert.IsType<ToolDock>(root.VisibleDockables[1]);
        Assert.Equal("LeftTools", tools.Id);
        Assert.Equal(2, tools.VisibleDockables.Count);
    }

    [Fact]
    public void ToDockTree_AllFourSideZones_CreateFourToolDocks()
    {
        var layout = new DockingLayout();
        DockingLayoutOps.Dock(layout, "l", DockZone.Left);
        DockingLayoutOps.Dock(layout, "r", DockZone.Right);
        DockingLayoutOps.Dock(layout, "t", DockZone.Top);
        DockingLayoutOps.Dock(layout, "b", DockZone.Bottom);
        DockingLayoutOps.Dock(layout, "c1", DockZone.Center);

        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(layout, factory, IdResolver());

        Assert.Equal(5, root.VisibleDockables.Count); // Documents + 4 tools
        Assert.IsType<DocumentDock>(root.VisibleDockables[0]);
        Assert.IsType<ToolDock>(root.VisibleDockables[1]);  // Left
        Assert.IsType<ToolDock>(root.VisibleDockables[2]);  // Right
        Assert.IsType<ToolDock>(root.VisibleDockables[3]);  // Top
        Assert.IsType<ToolDock>(root.VisibleDockables[4]);  // Bottom
    }

    [Fact]
    public void ToDockTree_FloatingEntry_SkippedFromTree()
    {
        var layout = new DockingLayout();
        DockingLayoutOps.Float(layout, "floating-tool", 100, 100, 800, 600);

        var factory = new StubFactory();
        var root = DockingLayoutBridge.ToDockTree(layout, factory, IdResolver());

        // Floating entries are not added to the tree (they'd need
        // a separate window — deferred to Step 5).
        Assert.Single(root.VisibleDockables); // Documents only
        var documents = (DocumentDock)root.VisibleDockables[0];
        Assert.Empty(documents.VisibleDockables);
    }

    [Fact]
    public void ToDockTree_ResolveReturnsNull_SkipsThatEntry()
    {
        // The resolver returns null for "missing" — bridge should
        // skip silently rather than crash. This protects against
        // stale sidecars referencing unregistered view models.
        var layout = new DockingLayout();
        DockingLayoutOps.Dock(layout, "present", DockZone.Center);
        DockingLayoutOps.Dock(layout, "missing", DockZone.Center);

        var factory = new StubFactory();
        var resolver = IdResolver();
        var wrapped = (System.Func<DockEntry, IDockable?>)((entry) =>
            entry.Id == "missing" ? null : resolver(entry));

        var root = DockingLayoutBridge.ToDockTree(layout, factory, wrapped);
        var documents = (DocumentDock)root.VisibleDockables[0];
        Assert.Single(documents.VisibleDockables);
        Assert.Equal("present", documents.VisibleDockables[0].Id);
    }
}