using System;

namespace PrimalEditor_Avalonia.Components;

public enum CameraProjection
{
    Perspective,
    Orthographic
}

public sealed class CameraComponent : Component
{
    private float _fieldOfViewDegrees = 60f;
    private float _orthographicSize = 5f;
    private float _nearPlane = .1f;
    private float _farPlane = 1000f;

    public CameraProjection Projection { get; set; } = CameraProjection.Perspective;
    public float FieldOfViewDegrees { get => _fieldOfViewDegrees; set => _fieldOfViewDegrees = value is > 0 and < 180 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float OrthographicSize { get => _orthographicSize; set => _orthographicSize = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float NearPlane { get => _nearPlane; set => _nearPlane = value > 0 && value < _farPlane ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float FarPlane { get => _farPlane; set => _farPlane = value > _nearPlane ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float ClearColorR { get; set; }
    public float ClearColorG { get; set; }
    public float ClearColorB { get; set; }
    public float ClearColorA { get; set; } = 1f;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSCamera(msEntity);
}
