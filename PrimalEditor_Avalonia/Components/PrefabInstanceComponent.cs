using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Marker that an entity in a scene is an instance of a Prefab.
/// Holds the Prefab asset's GUID plus a per-component override
/// map so the editor can later merge scene-time deltas back into
/// the source Prefab without losing them.
/// </summary>
public sealed class PrefabInstanceComponent : Component
{
    /// <summary>GUID of the source Prefab asset.</summary>
    public AssetGuid PrefabGuid { get; set; }

    /// <summary>Map of { entity-name → override JSON }.</summary>
    public Dictionary<string, string> Overrides { get; set; } = new();

    /// <summary>True when the editor has unsaved edits that diverge
    /// from the source prefab; cleared on Save.</summary>
    public bool HasOverrides => Overrides.Count > 0;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        throw new NotSupportedException("Prefab multi-selection is not yet implemented.");
}
