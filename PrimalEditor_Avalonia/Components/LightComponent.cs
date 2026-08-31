using System;

namespace PrimalEditor_Avalonia.Components;

public enum LightType
{
    Directional,
    Point,
    Spot
}

public sealed class LightComponent : Component
{
    private float _intensity = 1f;
    private float _range = 10f;

    public LightType LightType { get; set; } = LightType.Point;
    public float ColorR { get; set; } = 1f;
    public float ColorG { get; set; } = 1f;
    public float ColorB { get; set; } = 1f;
    public float Intensity { get => _intensity; set => _intensity = value >= 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float Range { get => _range; set => _range = value >= 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float UmbraAngle { get; set; }
    public float PenumbraAngle { get; set; }
    public bool CastShadow { get; set; } = true;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSLight(msEntity);
}
