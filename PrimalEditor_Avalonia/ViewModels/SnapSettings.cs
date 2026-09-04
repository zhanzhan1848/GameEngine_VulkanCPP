using System;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Gizmo snapping settings (plan §6.1 A2). Translate snaps the drag delta
/// to a world-space grid step; rotate snaps to an angle step. Ctrl inverts
/// <see cref="Enabled"/> temporarily (the "hold to bypass snap" convention).
/// </summary>
public class SnapSettings : ViewModelBase
{
    public static readonly float[] TranslateSteps = { 0.05f, 0.1f, 0.25f, 0.5f, 1f };
    public static readonly float[] RotateSteps = { 1f, 5f, 15f, 45f };

    private bool _enabled = true;
    public bool Enabled
    {
        get => _enabled;
        set { if (_enabled != value) { _enabled = value; OnPropertyChanged(nameof(Enabled)); } }
    }

    private int _translateStepIndex = 3; // default 0.5m
    public int TranslateStepIndex
    {
        get => _translateStepIndex;
        set
        {
            if (_translateStepIndex != value && value >= 0 && value < TranslateSteps.Length)
            {
                _translateStepIndex = value;
                OnPropertyChanged(nameof(TranslateStepIndex));
            }
        }
    }
    public float TranslateStep => TranslateSteps[_translateStepIndex];

    private int _rotateStepIndex = 2; // default 15°
    public int RotateStepIndex
    {
        get => _rotateStepIndex;
        set
        {
            if (_rotateStepIndex != value && value >= 0 && value < RotateSteps.Length)
            {
                _rotateStepIndex = value;
                OnPropertyChanged(nameof(RotateStepIndex));
            }
        }
    }
    public float RotateStep => RotateSteps[_rotateStepIndex];

    /// <summary>Effective translate snap for this drag gesture: snapping on
    /// unless Ctrl is held (which bypasses it), or snapping off unless Ctrl
    /// is held (temporary snap).</summary>
    public bool EffectiveTranslateSnap(bool ctrlDown) => _enabled != ctrlDown;

    /// <summary>Effective rotate snap for this drag gesture.</summary>
    public bool EffectiveRotateSnap(bool ctrlDown) => _enabled != ctrlDown;

    /// <summary>Quantize an accumulated drag delta to the translate grid.
    /// Applying the snapped delta (not snapping absolute positions) keeps
    /// relative offsets inside a multi-selection intact.</summary>
    public float SnapTranslateDelta(float accumulatedDelta) =>
        MathF.Round(accumulatedDelta / TranslateStep) * TranslateStep;

    /// <summary>Quantize an accumulated rotation delta to the angle step.</summary>
    public float SnapRotateDelta(float accumulatedDelta) =>
        MathF.Round(accumulatedDelta / RotateStep) * RotateStep;
}
