using System.Collections.Generic;
using System.Linq;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Translates between the project's flat <see cref="DockingLayout"/>
/// (entries with <see cref="DockZone"/>) and Dock.Avalonia's
/// <see cref="IRootDock"/> tree (RootDock + DocumentDock + ToolDock +
/// ProportionalDock).
///
/// Mapping (one-way, layout → tree, is the supported direction):
/// - <see cref="DockZone.Center"/> entries → tabs of a single
///   DocumentDock under RootDock
/// - <see cref="DockZone.Left"/>/<see cref="DockZone.Right"/> entries →
///   ToolDock siblings inside a ProportionalDock on the corresponding
///   side of RootDock
/// - <see cref="DockZone.Top"/>/<see cref="DockZone.Bottom"/> entries →
///   ToolDock siblings inside a ProportionalDock on the corresponding
///   edge
/// - Floating entries → not represented in the runtime tree (the
///   factory is responsible for managing floating windows separately;
///   see P1.d design doc Step 5 for the full-fidelity bridge)
///
/// Reverse mapping (tree → layout, future work) is not implemented.
/// The runtime tree is the source of truth while the editor is open;
/// the flat <see cref="DockingLayout"/> is a serializable snapshot
/// at editor close.
///
/// See Docs/plans/2026-07-30-p1d-docking-full-migration.md for the
/// full design rationale.
/// </summary>
public static class DockingLayoutBridge
{
    /// <summary>
    /// Build a fresh <see cref="IRootDock"/> tree from a
    /// <see cref="DockingLayout"/> using the given factory. The
    /// returned root is suitable for direct binding to
    /// <c>DockControl.Layout</c>.
    /// </summary>
    /// <param name="layout">Source layout (entries by zone).</param>
    /// <param name="factory">Factory used to create DocumentDock /
    ///   ToolDock / ProportionalDock instances and to instantiate
    ///   the user's actual view models via
    ///   <see cref="IFactoryExtensions.CreateDockable"/> or the
    ///   factory's dockable registry.</param>
    /// <param name="resolveDockable">
    /// Callback the bridge calls for each <see cref="DockEntry"/> to
    /// obtain the Dockable to insert. Pass-through: given the
    /// entry's <see cref="DockEntry.Id"/>, return the corresponding
    /// already-constructed Dockable from the caller's registry. The
    /// bridge does not construct Dockables itself — that's the
    /// factory's responsibility (avoids the bridge needing to know
    /// the Dockable type).
    /// </param>
    public static IRootDock ToDockTree(
        DockingLayout layout,
        IFactory factory,
        System.Func<DockEntry, IDockable?> resolveDockable)
    {
        var center = new DocumentDock
        {
            Id = "Documents",
            Title = "Documents",
            VisibleDockables = new List<IDockable>(),
        };
        var root = new RootDock
        {
            Id = "Root",
            Title = "Root",
            VisibleDockables = new List<IDockable> { center },
        };

        if (layout == null) return root;

        // Center zone → DocumentDock children, ordered by TabIndex.
        foreach (var entry in DockingLayoutOps.PanelsInZone(layout, DockZone.Center)
                     .OrderBy(e => e.TabIndex))
        {
            var dockable = resolveDockable(entry);
            if (dockable != null) center.VisibleDockables.Add(dockable);
        }

        // Left / Right / Top / Bottom zones → ToolDock wrapped in
        // ProportionalDock, positioned on the corresponding side of
        // the RootDock.
        AttachSide(root, DockZone.Left, "LeftTools", factory, layout, resolveDockable);
        AttachSide(root, DockZone.Right, "RightTools", factory, layout, resolveDockable);
        AttachSide(root, DockZone.Top, "TopTools", factory, layout, resolveDockable);
        AttachSide(root, DockZone.Bottom, "BottomTools", factory, layout, resolveDockable);

        // Floating entries are not represented in the tree; the
        // caller manages them via the factory's dockable registry.
        // Their bounds and active state are stored in DockingLayout
        // but not restored here.

        return root;
    }

    private static void AttachSide(
        IRootDock root,
        DockZone zone,
        string toolsDockId,
        IFactory factory,
        DockingLayout layout,
        System.Func<DockEntry, IDockable?> resolveDockable)
    {
        var entries = DockingLayoutOps.PanelsInZone(layout, zone)
            .OrderBy(e => e.TabIndex)
            .ToList();
        if (entries.Count == 0) return;

        var tools = new ToolDock
        {
            Id = toolsDockId,
            Title = zone.ToString(),
            VisibleDockables = new List<IDockable>(),
        };
        foreach (var entry in entries)
        {
            var dockable = resolveDockable(entry);
            if (dockable != null) tools.VisibleDockables.Add(dockable);
        }
        root.VisibleDockables.Add(tools);
    }

    /// <summary>
    /// Walk a live <see cref="IRootDock"/> tree and emit a flat
    /// <see cref="DockingLayout"/> snapshot. Reverse of
    /// <see cref="ToDockTree"/>; used to capture the current dock
    /// state for persistence (P1.d Step 6).
    ///
    /// Only <see cref="DocumentDock"/> children of the root are
    /// captured in this v1 — left/right/top/bottom zones
    /// (ProportionalDock siblings) are deferred to a later step
    /// because the runtime tree only has a single DocumentDock
    /// (MainWindowDockFactory). The function returns a non-null
    /// <see cref="DockingLayout"/> for any non-null root.
    /// </summary>
    public static DockingLayout FromDockTree(IRootDock root)
    {
        var layout = new DockingLayout();
        if (root == null) return layout;

        int tabIndex = 0;
        foreach (var rootChild in root.VisibleDockables)
        {
            switch (rootChild)
            {
                case DocumentDock docs:
                    foreach (var doc in docs.VisibleDockables)
                    {
                        if (doc == null) continue;
                        layout.Entries.Add(new DockEntry
                        {
                            Id = doc.Id ?? string.Empty,
                            Zone = DockZone.Center,
                            TabIndex = tabIndex++,
                        });
                    }
                    break;
                case ToolDock tools when tools.Alignment == Alignment.Left:
                    EmitToolDock(tools, layout, DockZone.Left);
                    break;
                case ToolDock tools when tools.Alignment == Alignment.Right:
                    EmitToolDock(tools, layout, DockZone.Right);
                    break;
                case ToolDock tools when tools.Alignment == Alignment.Top:
                    EmitToolDock(tools, layout, DockZone.Top);
                    break;
                case ToolDock tools when tools.Alignment == Alignment.Bottom:
                    EmitToolDock(tools, layout, DockZone.Bottom);
                    break;
            }
        }
        return layout;
    }

    private static void EmitToolDock(ToolDock tools, DockingLayout layout, DockZone zone)
    {
        int tabIndex = 0;
        foreach (var dockable in tools.VisibleDockables)
        {
            if (dockable == null) continue;
            layout.Entries.Add(new DockEntry
            {
                Id = dockable.Id ?? string.Empty,
                Zone = zone,
                TabIndex = tabIndex++,
            });
        }
    }
}