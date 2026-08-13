using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Entities;

/// <summary>
/// Declarative description of one user-facing entity type. The
/// <see cref="EntityFactory"/> registry is the single source of truth
/// for which toolbar buttons exist, what default values to apply, and
/// which components to attach. The view layer binds to
/// <see cref="EntityFactory.All"/> via ItemsControl; no view edits are
/// needed when adding a new entity type.
/// </summary>
public sealed record EntityFactoryDescriptor(
    EntityKind Kind,
    string DisplayName,
    string IconGlyph,
    string Tooltip,
    IReadOnlyList<string> DefaultComponentIds,
    Func<GameEntity> CreateEntity,
    bool RequiresAssetSelection = false);

/// <summary>
/// Static registry of every entity type the toolbar exposes. New kinds
/// go in <see cref="EntityKind"/> and one row here. The view-model
/// re-reads <see cref="All"/> at construction; tests assert on it.
/// </summary>
public static class EntityFactory
{
    public static IReadOnlyList<EntityFactoryDescriptor> All { get; } = BuildAll();

    private static IReadOnlyList<EntityFactoryDescriptor> BuildAll()
    {
        var characterCount = 0;
        var propCount = 0;
        var lightCount = 0;
        var cameraCount = 0;

        return new[]
        {
            new EntityFactoryDescriptor(
                EntityKind.Empty,
                "Entity",
                "+",
                "Blank entity: only a Transform",
                new[] { "transform" },
                () => new GameEntity("Entity") { Components = { new Transform() } }),

            new EntityFactoryDescriptor(
                EntityKind.Character,
                "Character",
                "👤",
                "Placeholder character: capsule primitive",
                new[] { "transform", "meshRenderer" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref characterCount);
                    var entity = new GameEntity($"Character_{n}");
                    entity.Components.Add(new Transform());
                    // MeshRenderer.MeshGuid left empty; PrimitiveMeshView
                    // cooks + assigns a capsule in a follow-up flow.
                    // Until that lands, the entity has no visual mesh
                    // — same as today's blank Entity.
                    entity.Components.Add(new MeshRendererComponent());
                    return entity;
                }),

            new EntityFactoryDescriptor(
                EntityKind.Prop,
                "Prop",
                "📦",
                "Pick a mesh asset from Content Browser",
                new[] { "transform", "meshRenderer" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref propCount);
                    var entity = new GameEntity($"Prop_{n}");
                    entity.Components.Add(new Transform());
                    entity.Components.Add(new MeshRendererComponent());
                    return entity;
                },
                RequiresAssetSelection: true),

            new EntityFactoryDescriptor(
                EntityKind.Light,
                "Light",
                "💡",
                "Point light (change type in Inspector)",
                new[] { "transform", "light" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref lightCount);
                    var entity = new GameEntity($"Light_{n}");
                    entity.Components.Add(new Transform());
                    entity.Components.Add(new LightComponent());
                    return entity;
                }),

            new EntityFactoryDescriptor(
                EntityKind.Camera,
                "Camera",
                "🎥",
                "Perspective camera (FOV/Near/Far editable in Inspector)",
                new[] { "transform", "camera" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref cameraCount);
                    var entity = new GameEntity($"Camera_{n}");
                    entity.Components.Add(new Transform());
                    entity.Components.Add(new CameraComponent());
                    return entity;
                }),
        };
    }

    public static EntityFactoryDescriptor? Find(EntityKind kind)
    {
        foreach (var d in All)
            if (d.Kind == kind) return d;
        return null;
    }
}
