using System;

namespace PrimalEditor_Avalonia.Entities;

/// <summary>
/// High-level classification of an entity the user can drop into a
/// scene. <see cref="EntityFactory"/> uses this to decide which
/// components to attach and what default values to use. Adding a new
/// entity type = one new enum value + one
/// <see cref="EntityFactoryDescriptor"/> row, no view changes.
/// </summary>
public enum EntityKind
{
    /// <summary>Blank entity with only a Transform (legacy "Add Entity").</summary>
    Empty,

    /// <summary>Placeholder character: capsule primitive + Transform.</summary>
    Character,

    /// <summary>Prop: chosen-from-browser mesh + Transform + MeshRenderer.</summary>
    Prop,

    /// <summary>Point light with default params, editable in Inspector.</summary>
    Light,

    /// <summary>Perspective camera with default params, editable in Inspector.</summary>
    Camera,
}
