using System;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Entities;

/// <summary>
/// Factory for entities every new scene starts with. Called from the
/// scene-creation paths (<see cref="GameProject.NewProject.Create"/> and
/// <c>WorldEditorViewModel.AddScene</c>) — NOT from the <c>Scene</c>
/// constructor, so deserialized scenes and tests keep full control over
/// their entity list.
/// </summary>
public static class SceneDefaults
{
    // Direction the default sun shines: from (100, 150, 50) toward the
    // origin, matching the renderer's legacy fallback light exactly.
    private const float SunPositionX = 100f;
    private const float SunPositionY = 150f;
    private const float SunPositionZ = 50f;

    // Euler angles (radians, engine order X→Y→Z) whose world Z column
    // equals normalize(-100, -150, -50) = (-0.535, -0.802, -0.267).
    // Public so toolbar-created suns start with the same "sunrise" tilt —
    // identity rotation shines horizontally and lights nothing on the ground.
    public const float SunRotationX = 0f;
    public const float SunRotationY = 1.8415f;
    public const float SunRotationZ = -2.1589f;

    // Legacy fallback light was RGB {20,20,20} with the intensity channel
    // dropped by the shader; Color(1,1,1) × Intensity(20) reproduces it.
    private const float SunIntensity = 20f;

    /// <summary>
    /// Applies the sun configuration (directional type, white color,
    /// legacy-fallback brightness, shadow casting) to a light component.
    /// Shared by the scene default sun and the toolbar "Sun Light" button
    /// so both produce identical lighting.
    /// </summary>
    public static void ConfigureSunLight(LightComponent light)
    {
        light.LightType = LightType.Directional;
        light.ColorR = 1f;
        light.ColorG = 1f;
        light.ColorB = 1f;
        light.Intensity = SunIntensity;
        light.CastShadow = true;
    }

    /// <summary>
    /// The directional "Sun" light every new scene starts with. Drives
    /// viewport lighting + cascade shadows and overrides the renderer's
    /// hardcoded fallback light.
    /// </summary>
    public static GameEntity CreateSunLight()
    {
        var entity = new GameEntity("Sun");
        entity.Components.Add(new Transform
        {
            PositionX = SunPositionX,
            PositionY = SunPositionY,
            PositionZ = SunPositionZ,
            RotationX = SunRotationX,
            RotationY = SunRotationY,
            RotationZ = SunRotationZ,
        });
        var light = new LightComponent();
        ConfigureSunLight(light);
        entity.Components.Add(light);
        return entity;
    }
}
