using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// UI-independent dock layout model. The view layer translates the
/// layout into actual Grids/Windows; this model is the single source
/// of truth for "which panel is where" and supports JSON
/// round-tripping via <see cref="DockingLayoutSerializer"/>.
///
/// A panel lives in exactly one zone at a time. Floating panels keep
/// their last-known bounds; docked panels ignore bounds. Multiple
/// panels per zone are stacked as tabs (the view layer arranges
/// them).
/// </summary>
public sealed class DockingLayout
{
    public List<DockEntry> Entries { get; set; } = new();
    public string? ActiveDockableId { get; set; }
    public int SchemaVersion { get; set; } = 1;
}

public enum DockZone { Left, Right, Top, Bottom, Center }

public sealed class DockEntry
{
    public string Id { get; set; } = string.Empty;
    public DockZone Zone { get; set; } = DockZone.Center;
    public bool IsFloating { get; set; }
    public float X { get; set; }
    public float Y { get; set; }
    public float Width { get; set; } = 600;
    public float Height { get; set; } = 400;
    public int TabIndex { get; set; }
}

/// <summary>
/// Pure functions that mutate a <see cref="DockingLayout"/> in place.
/// View-layer drag/float/dock handlers call into these instead of
/// editing the layout directly so the contract is testable.
/// </summary>
public static class DockingLayoutOps
{
    public static void Dock(DockingLayout layout, string id, DockZone zone)
    {
        if (layout == null) throw new ArgumentNullException(nameof(layout));
        if (string.IsNullOrEmpty(id)) throw new ArgumentException("id required", nameof(id));
        var entry = EnsureEntry(layout, id);
        entry.IsFloating = false;
        entry.Zone = zone;
        CompactTabs(layout, zone);
    }

    public static void Float(DockingLayout layout, string id, float x, float y, float width, float height)
    {
        if (layout == null) throw new ArgumentNullException(nameof(layout));
        if (string.IsNullOrEmpty(id)) throw new ArgumentException("id required", nameof(id));
        var entry = EnsureEntry(layout, id);
        entry.IsFloating = true;
        entry.X = x; entry.Y = y; entry.Width = width; entry.Height = height;
    }

    public static void CloseFloat(DockingLayout layout, string id)
    {
        if (layout == null) throw new ArgumentNullException(nameof(layout));
        if (string.IsNullOrEmpty(id)) throw new ArgumentException("id required", nameof(id));
        var entry = EnsureEntry(layout, id);
        if (!entry.IsFloating) return;
        entry.IsFloating = false;
        entry.Zone = DockZone.Center;
        CompactTabs(layout, DockZone.Center);
    }

    public static IEnumerable<DockEntry> PanelsInZone(DockingLayout layout, DockZone zone) =>
        layout.Entries.Where(e => !e.IsFloating && e.Zone == zone).OrderBy(e => e.TabIndex);

    public static int CountInZone(DockingLayout layout, DockZone zone) =>
        layout.Entries.Count(e => !e.IsFloating && e.Zone == zone);

    private static DockEntry EnsureEntry(DockingLayout layout, string id)
    {
        var entry = layout.Entries.FirstOrDefault(e => e.Id == id);
        if (entry != null) return entry;
        entry = new DockEntry { Id = id };
        layout.Entries.Add(entry);
        return entry;
    }

    private static void CompactTabs(DockingLayout layout, DockZone zone)
    {
        int idx = 0;
        foreach (var e in layout.Entries.Where(x => !x.IsFloating && x.Zone == zone))
        {
            e.TabIndex = idx++;
        }
    }
}
