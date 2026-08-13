using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject;

/// <summary>
/// Scene-owned editor viewport camera. It is editor-only state: scene camera
/// entities remain the cameras used by game/runtime rendering.
/// </summary>
public sealed class EditorCameraState : ViewModelBase
{
    private float _targetX;
    private float _targetY;
    private float _targetZ;
    private float _distance = 12f;
    private float _yaw;
    private float _pitch;

    public float TargetX { get => _targetX; set => Set(ref _targetX, value); }
    public float TargetY { get => _targetY; set => Set(ref _targetY, value); }
    public float TargetZ { get => _targetZ; set => Set(ref _targetZ, value); }
    public float Distance { get => _distance; set => Set(ref _distance, value); }
    public float Yaw { get => _yaw; set => Set(ref _yaw, value); }
    public float Pitch { get => _pitch; set => Set(ref _pitch, value); }

    public void CopyFrom(EditorCameraState source)
    {
        TargetX = source.TargetX;
        TargetY = source.TargetY;
        TargetZ = source.TargetZ;
        Distance = source.Distance;
        Yaw = source.Yaw;
        Pitch = source.Pitch;
    }

    private void Set(ref float field, float value)
    {
        if (field == value) return;
        field = value;
        OnPropertyChanged();
    }
}
