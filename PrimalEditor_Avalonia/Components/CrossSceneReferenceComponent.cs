using System;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Pins an entity in another scene by <see cref="EntityGuid"/>. The
/// target scene is resolved through the project's
/// <see cref="GameProject.Project.Scenes"/> collection at edit time
/// (and through whatever scene-binding the runtime exposes at play
/// time). The GUID stays stable across saves because it is
/// authored once and never re-derived.
///
/// Use cases: nav-mesh anchors that must follow a specific prop
/// across scene transitions; light-bake baking groups that span
/// the level boundary; cinematic cameras that move between scenes.
/// </summary>
public sealed class CrossSceneReferenceComponent : Component
{
    /// <summary>GUID of the entity being referenced.</summary>
    public EntityGuid Target { get; set; }

    /// <summary>Optional human-readable label so a Designer viewport
    /// can show "→ BossArenaDoor" rather than a raw Guid.</summary>
    public string Label { get; set; } = string.Empty;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        throw new NotSupportedException("Cross-scene multi-selection is not yet implemented.");
}
