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
        var sphereCount = 0;
        var boxCount = 0;
        var planeCount = 0;
        var lightCount = 0;
        var sunLightCount = 0;
        var envLightCount = 0;
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
                    entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Capsule });
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

            CreateBuiltinDescriptor(EntityKind.Sphere, "Sphere", "●", "Built-in procedural sphere",
                BuiltinMeshKind.Sphere, () => System.Threading.Interlocked.Increment(ref sphereCount)),

            CreateBuiltinDescriptor(EntityKind.Box, "Box", "▣", "Built-in procedural box",
                BuiltinMeshKind.Box, () => System.Threading.Interlocked.Increment(ref boxCount)),

            CreateBuiltinDescriptor(EntityKind.Plane, "Plane", "▱", "Built-in procedural plane",
                BuiltinMeshKind.Plane, () => System.Threading.Interlocked.Increment(ref planeCount)),

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
                EntityKind.DirectionalLight,
                "Sun Light",
                "☀",
                "Directional sun light (drives lighting + shadows)",
                new[] { "transform", "light" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref sunLightCount);
                    var entity = new GameEntity($"Sun_{n}");
                    // Start tilted like the scene-default sun — identity
                    // rotation shines horizontally along -Z and leaves
                    // upward-facing ground geometry unlit.
                    entity.Components.Add(new Transform
                    {
                        RotationX = SceneDefaults.SunRotationX,
                        RotationY = SceneDefaults.SunRotationY,
                        RotationZ = SceneDefaults.SunRotationZ,
                    });
                    var light = new LightComponent();
                    SceneDefaults.ConfigureSunLight(light);
                    entity.Components.Add(light);
                    return entity;
                }),

            new EntityFactoryDescriptor(
                EntityKind.EnvironmentLight,
                "Env Light",
                "🌍",
                "Environment light — edit HDR sky / IBL settings by selecting it",
                new[] { "transform", "environment" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref envLightCount);
                    var entity = new GameEntity($"Env_{n}");
                    entity.Components.Add(new Transform());
                    entity.Components.Add(new EnvironmentComponent());
                    return entity;
                }),

            new EntityFactoryDescriptor(
                EntityKind.EnvironmentLight,
                "Env Light",
                "🌍",
                "Environment light — edit HDR sky / IBL settings by selecting it",
                new[] { "transform", "environment" },
                () =>
                {
                    var n = System.Threading.Interlocked.Increment(ref envLightCount);
                    var entity = new GameEntity($"Env_{n}");
                    entity.Components.Add(new Transform());
                    entity.Components.Add(new EnvironmentComponent());
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

    private static EntityFactoryDescriptor CreateBuiltinDescriptor(
        EntityKind kind,
        string displayName,
        string glyph,
        string tooltip,
        BuiltinMeshKind mesh,
        Func<int> nextNumber)
    {
        return new EntityFactoryDescriptor(
            kind,
            displayName,
            glyph,
            tooltip,
            new[] { "transform", "meshRenderer" },
            () =>
            {
                var entity = new GameEntity($"{displayName}_{nextNumber()}");
                entity.Components.Add(new Transform());
                entity.Components.Add(new MeshRendererComponent { BuiltinMesh = mesh });
                return entity;
            });
    }

    public static EntityFactoryDescriptor? Find(EntityKind kind)
    {
        foreach (var d in All)
            if (d.Kind == kind) return d;
        return null;
    }
}
