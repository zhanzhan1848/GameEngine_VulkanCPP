using System;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class PropertiesViewModelMaterialTests
{
    [Fact]
    public void MeshSelection_AlwaysExposesAnEmptyMaterialDropSlot()
    {
        var entity = Mesh("Sphere", BuiltinMeshKind.Sphere);
        var properties = new PropertiesViewModel { Entities = new[] { entity } };

        var slot = Assert.Single(properties.MeshMaterials);
        Assert.Equal(0, slot.Index);
        Assert.False(slot.HasMaterial);
        Assert.Equal("Drop material here", slot.DisplayName);
        Assert.Equal("Built-in Sphere", properties.MeshAsset!.DisplayName);
    }

    [Fact]
    public void AssignMeshAsset_ReplacesBuiltinGeometryAcrossSelectionAndRefreshesRuntime()
    {
        var sphere = Mesh("Sphere", BuiltinMeshKind.Sphere);
        var box = Mesh("Box", BuiltinMeshKind.Box);
        var pushed = Array.Empty<GameEntity>();
        var dirty = false;
        var properties = new PropertiesViewModel
        {
            Entities = new[] { sphere, box },
            PushMeshesToNativeCallback = entities => pushed = entities.ToArray(),
            MarkProjectDirtyCallback = () => dirty = true,
        };
        var meshGuid = Guid.NewGuid();

        var assigned = properties.AssignMeshAsset(new AssetInfo
        {
            Guid = meshGuid,
            FullPath = "/project/Content/Models/Chair.model.json",
            Type = AssetType.Model,
        });

        Assert.True(assigned);
        Assert.All(new[] { sphere, box }, entity =>
        {
            var renderer = entity.GetComponent<MeshRendererComponent>()!;
            Assert.Equal(meshGuid, renderer.MeshGuid.Value);
            Assert.Equal(BuiltinMeshKind.None, renderer.BuiltinMesh);
        });
        Assert.Equal(new[] { sphere, box }, pushed);
        Assert.True(dirty);
    }

    [Fact]
    public void ClearMeshAsset_RemovesGeometryButKeepsRendererAndMaterials()
    {
        var entity = Mesh("Plane", BuiltinMeshKind.Plane);
        var material = new AssetGuid(Guid.NewGuid());
        entity.GetComponent<MeshRendererComponent>()!.MaterialGuids.Add(material);
        var pushed = Array.Empty<GameEntity>();
        var properties = new PropertiesViewModel
        {
            Entities = new[] { entity },
            PushMeshesToNativeCallback = entities => pushed = entities.ToArray(),
        };

        Assert.True(properties.ClearMeshAsset());

        var renderer = entity.GetComponent<MeshRendererComponent>()!;
        Assert.Equal(Guid.Empty, renderer.MeshGuid.Value);
        Assert.Equal(BuiltinMeshKind.None, renderer.BuiltinMesh);
        Assert.Equal(material, renderer.MaterialGuids[0]);
        Assert.Equal(new[] { entity }, pushed);
        Assert.Equal("Drop Mesh asset here", properties.MeshAsset!.DisplayName);
    }

    [Fact]
    public void AssignMeshAsset_RejectsTextureAsset()
    {
        var entity = Mesh("Sphere", BuiltinMeshKind.Sphere);
        var properties = new PropertiesViewModel { Entities = new[] { entity } };

        Assert.False(properties.AssignMeshAsset(new AssetInfo
        {
            Guid = Guid.NewGuid(),
            FullPath = "/project/Content/Textures/Test.texture",
            Type = AssetType.Texture,
        }));
        Assert.Equal(BuiltinMeshKind.Sphere, entity.GetComponent<MeshRendererComponent>()!.BuiltinMesh);
    }

    [Fact]
    public void AssignMeshMaterial_AppliesToEverySelectedMeshAndRefreshesRuntime()
    {
        var sphere = Mesh("Sphere", BuiltinMeshKind.Sphere);
        var box = Mesh("Box", BuiltinMeshKind.Box);
        var pushed = Array.Empty<GameEntity>();
        var dirty = false;
        var properties = new PropertiesViewModel
        {
            Entities = new[] { sphere, box },
            PushMaterialsToNativeCallback = entities => pushed = entities.ToArray(),
            MarkProjectDirtyCallback = () => dirty = true,
        };
        var materialGuid = Guid.NewGuid();

        var assigned = properties.AssignMeshMaterial(0, new AssetInfo
        {
            Guid = materialGuid,
            FullPath = "/project/Content/Materials/Test.material.json",
            Type = AssetType.Material,
        });

        Assert.True(assigned);
        Assert.Equal(materialGuid, sphere.GetComponent<MeshRendererComponent>()!.MaterialGuids[0].Value);
        Assert.Equal(materialGuid, box.GetComponent<MeshRendererComponent>()!.MaterialGuids[0].Value);
        Assert.Equal(new[] { sphere, box }, pushed);
        Assert.True(dirty);
        Assert.True(properties.MeshMaterials[0].HasMaterial);
    }

    [Fact]
    public void AssignMeshMaterial_RejectsNonMaterialAssets()
    {
        var sphere = Mesh("Sphere", BuiltinMeshKind.Sphere);
        var properties = new PropertiesViewModel { Entities = new[] { sphere } };

        var assigned = properties.AssignMeshMaterial(0, new AssetInfo
        {
            Guid = Guid.NewGuid(),
            FullPath = "/project/Content/Textures/Test.texture",
            Type = AssetType.Texture,
        });

        Assert.False(assigned);
        Assert.Empty(sphere.GetComponent<MeshRendererComponent>()!.MaterialGuids);
    }

    private static GameEntity Mesh(string name, BuiltinMeshKind kind)
    {
        var entity = new GameEntity(name);
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = kind });
        return entity;
    }
}
