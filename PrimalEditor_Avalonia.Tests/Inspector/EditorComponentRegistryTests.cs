using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Inspector;

public sealed class EditorComponentRegistryTests
{
    public EditorComponentRegistryTests()
    {
        EditorComponentRegistry.Clear();
    }

    [Fact]
    public void Register_StoresDescriptor()
    {
        var d = MakeDescriptor("transform", "Transform", typeof(Transform));
        EditorComponentRegistry.Register(d);
        Assert.True(EditorComponentRegistry.TryGet("transform", out var got));
        Assert.Same(d, got);
    }

    [Fact]
    public void Register_DuplicateId_Throws()
    {
        EditorComponentRegistry.Register(MakeDescriptor("a", "A", typeof(Transform)));
        Assert.Throws<InvalidOperationException>(() =>
            EditorComponentRegistry.Register(MakeDescriptor("a", "A2", typeof(Transform))));
    }

    [Fact]
    public void Factory_ProducesNewInstance()
    {
        EditorComponentRegistry.Register(MakeDescriptor("transform", "Transform", typeof(Transform)));
        Assert.True(EditorComponentRegistry.TryGet("transform", out var d));
        var c = d.Factory();
        Assert.NotNull(c);
        Assert.IsType<Transform>(c);
    }

    [Fact]
    public void RequiredFlag_Roundtrips()
    {
        EditorComponentRegistry.Register(MakeDescriptor("transform", "Transform", typeof(Transform), required: true));
        Assert.True(EditorComponentRegistry.TryGet("transform", out var d));
        Assert.True(d.Required);
    }

    [Fact]
    public void Clear_RemovesEverything()
    {
        EditorComponentRegistry.Register(MakeDescriptor("a", "A", typeof(Transform)));
        EditorComponentRegistry.Clear();
        Assert.Empty(EditorComponentRegistry.All);
    }

    [Fact]
    public void All_ExposesRegisteredDescriptors()
    {
        EditorComponentRegistry.Register(MakeDescriptor("a", "A", typeof(Transform)));
        EditorComponentRegistry.Register(MakeDescriptor("b", "B", typeof(Transform)));
        Assert.Equal(2, EditorComponentRegistry.All.Count);
    }

    private static EditorComponentDescriptor MakeDescriptor(string id, string name, Type t, bool required = false) =>
        new(id, name, t, () => (Component)Activator.CreateInstance(t)!, required);
}
