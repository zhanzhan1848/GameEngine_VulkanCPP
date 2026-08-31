using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Entities;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Entities;

public sealed class EntityFactoryTests
{
    [Fact]
    public void All_ContainsBaseAndBuiltinKinds()
    {
        var kinds = EntityFactory.All.Select(d => d.Kind).ToHashSet();
        Assert.Contains(EntityKind.Empty, kinds);
        Assert.Contains(EntityKind.Character, kinds);
        Assert.Contains(EntityKind.Prop, kinds);
        Assert.Contains(EntityKind.Sphere, kinds);
        Assert.Contains(EntityKind.Box, kinds);
        Assert.Contains(EntityKind.Plane, kinds);
        Assert.Contains(EntityKind.Light, kinds);
        Assert.Contains(EntityKind.Camera, kinds);
    }

    [Fact]
    public void Empty_AttachesOnlyTransform()
    {
        var entity = EntityFactory.Find(EntityKind.Empty)!.CreateEntity();
        Assert.Single(entity.Components);
        Assert.IsType<Transform>(entity.Components[0]);
    }

    [Fact]
    public void Light_AttachesLightComponent()
    {
        var entity = EntityFactory.Find(EntityKind.Light)!.CreateEntity();
        Assert.Contains(entity.Components.OfType<LightComponent>(), c => true);
        Assert.IsType<LightComponent>(entity.GetComponent<LightComponent>());
    }

    [Fact]
    public void DirectionalLight_AttachesDirectionalSunLight()
    {
        var descriptor = EntityFactory.Find(EntityKind.DirectionalLight);
        Assert.NotNull(descriptor);
        Assert.False(descriptor!.RequiresAssetSelection);

        var entity = descriptor.CreateEntity();
        Assert.IsType<Transform>(entity.Components[0]);
        var light = entity.GetComponent<LightComponent>();
        Assert.NotNull(light);
        Assert.Equal(LightType.Directional, light!.LightType);
        // Toolbar sun matches the scene-default sun (SceneDefaults.ConfigureSunLight).
        Assert.Equal(20f, light.Intensity);
        Assert.True(light.CastShadow);
        Assert.StartsWith("Sun_", entity.Name);
    }

    [Fact]
    public void DirectionalLightNaming_AutoIncrements()
    {
        var descriptor = EntityFactory.Find(EntityKind.DirectionalLight)!;
        var a = descriptor.CreateEntity();
        var b = descriptor.CreateEntity();
        Assert.StartsWith("Sun_", a.Name);
        Assert.StartsWith("Sun_", b.Name);
        Assert.NotEqual(a.Name, b.Name);
    }

    [Fact]
    public void Camera_AttachesCameraComponent()
    {
        var entity = EntityFactory.Find(EntityKind.Camera)!.CreateEntity();
        Assert.IsType<CameraComponent>(entity.GetComponent<CameraComponent>());
    }

    [Fact]
    public void Character_AttachesMeshRenderer()
    {
        var entity = EntityFactory.Find(EntityKind.Character)!.CreateEntity();
        Assert.NotNull(entity.GetComponent<MeshRendererComponent>());
        Assert.IsType<Transform>(entity.Components[0]);
        Assert.Equal(BuiltinMeshKind.Capsule, entity.GetComponent<MeshRendererComponent>()!.BuiltinMesh);
    }

    [Theory]
    [InlineData(EntityKind.Sphere, BuiltinMeshKind.Sphere)]
    [InlineData(EntityKind.Box, BuiltinMeshKind.Box)]
    [InlineData(EntityKind.Plane, BuiltinMeshKind.Plane)]
    public void BuiltinMeshKinds_AttachExpectedProceduralMesh(EntityKind kind, BuiltinMeshKind mesh)
    {
        var entity = EntityFactory.Find(kind)!.CreateEntity();
        Assert.IsType<Transform>(entity.Components[0]);
        Assert.Equal(mesh, entity.GetComponent<MeshRendererComponent>()!.BuiltinMesh);
        Assert.Equal(System.Guid.Empty, entity.GetComponent<MeshRendererComponent>()!.MeshGuid.Value);
    }

    [Fact]
    public void Prop_RequiresAssetSelection()
    {
        Assert.True(EntityFactory.Find(EntityKind.Prop)!.RequiresAssetSelection);
        Assert.False(EntityFactory.Find(EntityKind.Empty)!.RequiresAssetSelection);
        Assert.False(EntityFactory.Find(EntityKind.Light)!.RequiresAssetSelection);
    }

    [Fact]
    public void LightNaming_AutoIncrements()
    {
        var descriptor = EntityFactory.Find(EntityKind.Light)!;
        var a = descriptor.CreateEntity();
        var b = descriptor.CreateEntity();
        var c = descriptor.CreateEntity();
        Assert.StartsWith("Light_", a.Name);
        Assert.StartsWith("Light_", b.Name);
        Assert.StartsWith("Light_", c.Name);
        Assert.NotEqual(a.Name, b.Name);
        Assert.NotEqual(b.Name, c.Name);
    }

    [Fact]
    public void EachKind_HasNonEmptyDisplayNameAndGlyph()
    {
        foreach (var d in EntityFactory.All)
        {
            Assert.False(string.IsNullOrWhiteSpace(d.DisplayName));
            Assert.False(string.IsNullOrWhiteSpace(d.IconGlyph));
            Assert.False(string.IsNullOrWhiteSpace(d.Tooltip));
        }
    }

    [Fact]
    public void Find_ReturnsNullForUnknownKind()
    {
        Assert.Null(EntityFactory.Find((EntityKind)999));
    }
}
