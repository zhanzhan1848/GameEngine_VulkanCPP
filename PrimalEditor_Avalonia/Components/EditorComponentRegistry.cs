using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// One registered component type: a stable id, a human-readable
/// display name, the concrete <see cref="Component"/> subclass it
/// produces, and a factory that creates a fresh instance. The
/// registry is the only place the inspector and Add-Component dialog
/// look at — concrete ViewModels depend on the registry, not on
/// hardcoded component lists.
/// </summary>
public sealed class EditorComponentDescriptor
{
    public string Id { get; }
    public string DisplayName { get; }
    public Type ComponentType { get; }
    public Func<Component> Factory { get; }
    public bool Required { get; }

    public EditorComponentDescriptor(string id, string displayName, Type componentType, Func<Component> factory, bool required = false)
    {
        Id = id;
        DisplayName = displayName;
        ComponentType = componentType;
        Factory = factory;
        Required = required;
    }
}

public static class EditorComponentRegistry
{
    private static readonly Dictionary<string, EditorComponentDescriptor> _byId = new(StringComparer.Ordinal)
    {
        ["transform"] = new("transform", "Transform", typeof(Transform), () => new Transform(), required: true),
        ["meshRenderer"] = new("meshRenderer", "Mesh Renderer", typeof(MeshRendererComponent), () => new MeshRendererComponent()),
        ["light"] = new("light", "Light", typeof(LightComponent), () => new LightComponent()),
        ["camera"] = new("camera", "Camera", typeof(CameraComponent), () => new CameraComponent())
    };

    public static IReadOnlyCollection<EditorComponentDescriptor> All => _byId.Values;

    public static void Register(EditorComponentDescriptor descriptor)
    {
        if (descriptor == null) throw new ArgumentNullException(nameof(descriptor));
        if (_byId.ContainsKey(descriptor.Id))
            throw new InvalidOperationException($"Component id already registered: {descriptor.Id}");
        _byId[descriptor.Id] = descriptor;
    }

    public static bool TryGet(string id, out EditorComponentDescriptor descriptor)
    {
        if (_byId.TryGetValue(id, out var d)) { descriptor = d; return true; }
        descriptor = null!;
        return false;
    }

    public static void Clear() => _byId.Clear();
}
