using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// New scenes must start with the default sun (directional) light — the
/// entity that replaces the renderer's hardcoded fallback light.
/// </summary>
public sealed class SceneDefaultsTests
{
    [Fact]
    public void CreateSunLight_IsDirectionalWithLegacyFallbackColorAndDirection()
    {
        var sun = SceneDefaults.CreateSunLight();

        Assert.Equal("Sun", sun.Name);
        var transform = sun.GetComponent<Transform>();
        Assert.NotNull(transform);
        // Position matches the legacy fallback light position.
        Assert.Equal(100f, transform!.PositionX);
        Assert.Equal(150f, transform.PositionY);
        Assert.Equal(50f, transform.PositionZ);
        // Rotation (radians) points the entity Z column along
        // normalize(-100, -150, -50) — the legacy fallback light direction.
        Assert.Equal(0f, transform.RotationX);
        Assert.Equal(1.8415f, transform.RotationY, 3);
        Assert.Equal(-2.1589f, transform.RotationZ, 3);

        var light = sun.GetComponent<LightComponent>();
        Assert.NotNull(light);
        Assert.Equal(LightType.Directional, light!.LightType);
        Assert.Equal(1f, light.ColorR);
        Assert.Equal(1f, light.ColorG);
        Assert.Equal(1f, light.ColorB);
        // Legacy fallback was RGB {20,20,20} with the shader dropping
        // intensity — Color(1,1,1) × Intensity(20) reproduces it.
        Assert.Equal(20f, light.Intensity);
        Assert.True(light.CastShadow);
        Assert.True(light.IsEnabled);
    }

    [Fact]
    public void AddScene_CreatesSceneWithDefaultSunLight()
    {
        var project = Project.CreateNew("Test", "/tmp/test-project");
        var vm = new WorldEditorViewModel { Project = project };

        vm.AddScene("New Scene");

        var scene = project.Scenes.Single();
        var sun = Assert.IsType<GameEntity>(Assert.Single(scene.Entities));
        Assert.Equal("Sun", sun.Name);
        Assert.NotNull(sun.GetComponent<LightComponent>());
    }

    [Fact]
    public void NewProjectCreate_DefaultSceneContainsSunLight()
    {
        var root = Path.Combine(
            Path.GetTempPath(), "primal-sun-tests", Guid.NewGuid().ToString("N"));
        try
        {
            var project = NewProject.Create("SunProject", root, new ProjectTemplate());

            var scene = Assert.Single(project.Scenes);
            var sun = Assert.Single(scene.Entities);
            Assert.Equal("Sun", sun.Name);
            Assert.Equal(LightType.Directional, sun.GetComponent<LightComponent>()!.LightType);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }
}
