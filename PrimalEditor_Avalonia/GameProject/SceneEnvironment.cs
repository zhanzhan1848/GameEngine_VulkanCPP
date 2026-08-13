using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject;

/// <summary>
/// Editable runtime model for <see cref="Documents.SceneEnvironmentDocument"/>.
/// Mirrors the document fields 1:1 and raises property-change events so
/// the inspector + native push fire when the user edits values. Owns
/// dirty state via <see cref="IDirtyTrackable"/>.
/// </summary>
public sealed class SceneEnvironment : ViewModelBase, IDirtyTrackable
{
    private float _ambientR = 0.05f;
    private float _ambientG = 0.05f;
    private float _ambientB = 0.05f;
    private float _ambientIntensity = 1.0f;

    private bool _fogEnabled;
    private float _fogR = 0.7f;
    private float _fogG = 0.75f;
    private float _fogB = 0.8f;
    private float _fogNear = 10f;
    private float _fogFar = 200f;
    private float _fogDensity = 0.01f;

    private bool _skyboxEnabled;
    private float _skyR = 0.2f;
    private float _skyG = 0.4f;
    private float _skyB = 0.8f;

    private ToneMappingMode _toneMapping = ToneMappingMode.ACES;
    private float _exposure = 1.0f;

    private System.Guid? _mainCameraEntityGuid;

    public float AmbientR { get => _ambientR; set => Set(ref _ambientR, value); }
    public float AmbientG { get => _ambientG; set => Set(ref _ambientG, value); }
    public float AmbientB { get => _ambientB; set => Set(ref _ambientB, value); }
    public float AmbientIntensity { get => _ambientIntensity; set => Set(ref _ambientIntensity, value); }

    public bool FogEnabled { get => _fogEnabled; set => Set(ref _fogEnabled, value); }
    public float FogR { get => _fogR; set => Set(ref _fogR, value); }
    public float FogG { get => _fogG; set => Set(ref _fogG, value); }
    public float FogB { get => _fogB; set => Set(ref _fogB, value); }
    public float FogNear { get => _fogNear; set => Set(ref _fogNear, value); }
    public float FogFar { get => _fogFar; set => Set(ref _fogFar, value); }
    public float FogDensity { get => _fogDensity; set => Set(ref _fogDensity, value); }

    public bool SkyboxEnabled { get => _skyboxEnabled; set => Set(ref _skyboxEnabled, value); }
    public float SkyR { get => _skyR; set => Set(ref _skyR, value); }
    public float SkyG { get => _skyG; set => Set(ref _skyG, value); }
    public float SkyB { get => _skyB; set => Set(ref _skyB, value); }

    public ToneMappingMode ToneMapping { get => _toneMapping; set => Set(ref _toneMapping, value); }
    public float Exposure { get => _exposure; set => Set(ref _exposure, value); }

    public System.Guid? MainCameraEntityGuid
    {
        get => _mainCameraEntityGuid;
        set => Set(ref _mainCameraEntityGuid, value);
    }

    private void Set<T>(ref T field, T value)
    {
        if (!System.Collections.Generic.EqualityComparer<T>.Default.Equals(field, value))
        {
            field = value;
            MarkDirty();
        }
    }

    private bool _isDirty;
    public bool IsDirty
    {
        get => _isDirty;
        private set
        {
            if (_isDirty != value)
            {
                _isDirty = value;
                OnPropertyChanged();
            }
        }
    }

    public void MarkDirty() => IsDirty = true;
    public void ClearDirty() => IsDirty = false;
}

public enum ToneMappingMode
{
    None,
    Reinhard,
    ACES,
    ExposureOnly,
}